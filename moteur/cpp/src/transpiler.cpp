#include "transpiler.h"
#include "rir.h"
#include "rose_runtime.h"
#include <sstream>
#include <set>
#include <functional>
#include <map>
#include <cstdlib>
#include <cerrno>

// Map RirType to C++ type name.
static std::string cpp_type(RirType t) {
    switch (t) {
        case RirQ: return "rose_qd";
        case RirZ: return "rose_ent";
        case RirF: return "rose_float";
        case RirB: return "rose_bool";
        case RirS: return "rose_str";
        case RirU: return "rose_val";  // generic Valeur
        case RirTab: return "rose_tampon";
        case RirListe: return "rose_liste";
        case RirGd: return "rose_gd";
        case RirTbl: return "rose_table";   // M4.2
        case RirDyn: return "rose_dyn";     // M4.2
        default: return "rose_ent";
    }
}

// Vrai si le texte d'une constante entière ne tient pas dans un int64.
static bool cst_hors_int64(const std::string &txt) {
    if (txt.empty()) return false;
    errno = 0;
    char *fin = nullptr;
    (void)std::strtoll(txt.c_str(), &fin, 10);
    return errno == ERANGE || (fin && *fin != '\0');
}

// Vrai si une constante entière doit vivre en grand entier : hors int64,
// ou aux marges de l'arithmétique machine (|v| >= 2^62). L'interprète tient
// tout en GMP exact ; les valeurs proches des bornes débordent dès qu'on
// les combine (a-1, a*a, 2*a). En dessous du seuil, l'int64 rapide suffit
// aux programmes usuels ; au-dessus, on choisit l'exactitude GMP.
static bool cst_promu_gd(const std::string &txt) {
    if (cst_hors_int64(txt)) return true;
    size_t i = (txt[0] == '-' || txt[0] == '+') ? 1 : 0;
    unsigned __int128 v = 0;
    for (; i < txt.size(); i++) {
        if (txt[i] < '0' || txt[i] > '9') return false;
        if (v > (((__int128 unsigned)1 << 126))) return true;
        v = v * 10 + (unsigned)(txt[i] - '0');
    }
    return v >= ((__int128 unsigned)1 << 62);
}

// Map a RirOp to a C++ expression.
static std::string sanitize(const std::string &name);
static std::string nom_visible(const std::string &name);
static std::string cpp_operand(const RirOp &op, std::map<std::string,RirType> &temp_types,
                               const std::set<std::string> *flagpees = nullptr) {
    if (op.kind == RirOp::Const) {
        // Numeric constant: @c123 → 123
        if (op.name.rfind("@c", 0) == 0) {
            std::string txt = op.name.substr(2);
            // Check if it's a float
            if (op.ty == RirF) return txt + ".0"; // approximate
            if (op.ty == RirQ || (txt.find('/') != std::string::npos && txt.find_first_of("0123456789-") == 0
                    && txt.find_first_not_of("0123456789-/") == std::string::npos))
                return "rose_qd(\"" + txt + "\")";
            if (cst_promu_gd(txt)) return "rose_gd(\"" + txt + "\")";
            return txt;
        }
        // String constant: raw text → "text"
        if (op.ty == RirS) {
            // Escape the string
            std::string s = op.name;
            std::string escaped;
            for (char c : s) {
                if (c == '"') escaped += "\\\"";
                else if (c == '\\') escaped += "\\\\";
                else if (c == '\n') escaped += "\\n";
                else escaped += c;
            }
            return "\"" + escaped + "\"";
        }
        // Bool constant
        if (op.name == "vrai") return "true";
        if (op.name == "faux") return "false";
        return "0";
    }
    if (op.kind == RirOp::Temp) {
        std::string s = sanitize(op.name);  // %t0 → r_t0
        if (flagpees && flagpees->count(s))
            return "rose_lit(" + s + ", _def_" + s + ", \"" + nom_visible(op.name) + "\")";
        return s;
    }
    if (op.kind == RirOp::Global) {
        std::string s = sanitize(op.name);  // $var → r_var
        if (flagpees && flagpees->count(s))
            return "rose_lit(" + s + ", _def_" + s + ", \"" + nom_visible(op.name) + "\")";
        return s;
    }
    return "0";
}

// Sanitize a temp name for C++ (%t0 → r_t0)
static std::string sanitize(const std::string &name) {
    std::string s = name;
    // Remove % prefix
    if (!s.empty() && s[0] == '%') s = s.substr(1);
    // Remove $ prefix
    if (!s.empty() && s[0] == '$') s = s.substr(1);
    // Replace non-alphanumeric
    for (char &c : s) {
        if (!isalnum((unsigned char)c) && c != '_') c = '_';
    }
    // v3.14 — préfixe uniforme : un nom Rose est UN ESPACE LIBRE, il peut
    // porter un mot-clé C++ ($int, fn double) ou collisionner l'API du
    // runtime (rose_add…). Le binaire généré ne doit jamais casser g++
    // (contrat : émettre ou rejeter poliment). r_ isole tout, partout.
    return s.empty() ? "r_vide" : "r_" + s;
}

// Nom visible par l'utilisateur (sans préfixe %/$) — pour les messages
// d'erreur que l'AOT doit rendre OCTET pour OCTET identiques au cœur.
static std::string nom_visible(const std::string &name) {
    std::string s = name;
    if (!s.empty() && (s[0] == '%' || s[0] == '$')) s = s.substr(1);
    return s;
}

std::string transpiler_rir(const RirModule &mod) {
    std::ostringstream out;
    out << "#include \"rose_runtime.h\"\n";
    out << "#include <cstdint>\n";
    out << "#include <cstdio>\n";
    out << "#include <string>\n\n";

    // Noms des fonctions utilisateur (pour dispatcher les appels)
    std::set<std::string> user_fns;
    std::map<std::string, std::vector<RirType>> fn_ptys;   // types declarés des parametres
    std::map<std::string, RirType> fn_rets;                // type de retour declare
    for (auto &f : mod.fonctions) {
        if (f.nom == "$principal" || f.nom == "principal") continue;
        user_fns.insert(sanitize(f.nom));
        std::vector<RirType> pt;
        for (auto &p : f.params) pt.push_back(p.second);
        fn_ptys[sanitize(f.nom)] = pt;
        fn_rets[sanitize(f.nom)] = f.retour;
    }
    // Récursion (cycle dans le graphe d'appels) : lever la pile au démarrage.
    std::map<std::string, std::set<std::string>> graphe;
    for (auto &f : mod.fonctions) {
        if (f.nom == "$principal" || f.nom == "principal") continue;
        std::set<std::string> s;
        for (auto &b : f.blocs) for (auto &i : b.instrs) {
            if (i.op == "call" && !i.args.empty() && i.args[0].name[0] == '$' &&
                user_fns.count(sanitize(i.args[0].name.substr(1))))
                s.insert(sanitize(i.args[0].name.substr(1)));
        }
        graphe[sanitize(f.nom)] = s;
    }
    bool recursif = false;
    {
        std::function<bool(const std::string &, std::set<std::string> &, std::set<std::string> &)> dfs =
            [&](const std::string &n, std::set<std::string> &visites, std::set<std::string> &pile) -> bool {
            pile.insert(n);
            for (auto &m : graphe[n]) {
                if (pile.count(m)) return true;
                if (!visites.count(m)) { visites.insert(m); if (dfs(m, visites, pile)) return true; }
            }
            pile.erase(n);
            return false;
        };
        std::set<std::string> visites;
        for (auto &kv : graphe)
            if (!visites.count(kv.first)) { visites.insert(kv.first);
                std::set<std::string> pile;
                if (dfs(kv.first, visites, pile)) { recursif = true; break; } }
    }


    // Forward declare functions
    for (auto &f : mod.fonctions) {
        if (f.nom == "$principal" || f.nom == "principal") continue;
        // Determine return type
        std::string ret = cpp_type(f.retour);
        if (ret == "rose_val") ret = "rose_ent"; // default
        out << ret << " " << sanitize(f.nom) << "(";
        for (size_t i = 0; i < f.params.size(); i++) {
            if (i > 0) out << ", ";
            // v3.14.1 — les listes voyagent par RÉFÉRENCE : l'interprète les
            // partage (shared_ptr), un joindre() dans l'appelée doit être
            // visible de l'appelant.
            if (f.params[i].second == RirListe) out << "rose_liste& ";
            else out << cpp_type(f.params[i].second) << " ";
            out << sanitize(f.params[i].first);
        }
        out << ");\n";
    }
    out << "\n";

    // Generate function bodies
    for (auto &f : mod.fonctions) {
        std::string fname = (f.nom == "$principal" || f.nom == "principal") ? "main" : sanitize(f.nom);
        std::string ret = cpp_type(f.retour);
        if (ret == "rose_val") ret = "rose_ent";
        if (fname == "main") ret = "int";

        out << ret << " " << fname << "(";
        if (fname == "main") {
            out << "int argc, char **argv";
        } else {
            for (size_t i = 0; i < f.params.size(); i++) {
                if (i > 0) out << ", ";
                // v3.14.1 — listes par référence (voir déclaration avancée).
                if (f.params[i].second == RirListe) out << "rose_liste& ";
                else out << cpp_type(f.params[i].second) << " ";
                out << sanitize(f.params[i].first);
            }
        }
        out << ") {\n";

        if (fname == "main") {
            if (recursif) out << "    rose_pile_geante(argc, argv);\n";
            out << "    rose_runtime_set_args(argc, argv);\n";
        }

        // Collect temp variables
        std::set<std::string> temps;
        std::map<std::string, RirType> temp_types;
        // v3.14.1 — les paramètres SONT des variables du corps de fonction :
        // on amorce la carte avant toute lecture pour que toutes les
        // résolutions par nom (longueur, index, casts, promotions gd/flottant,
        // clés de table…) les voient, comme l'interprète. Sans cet amorce,
        // longueur(paramètre chaine) émettait .size() — des OCTETS — là où
        // le global correct émettait rose_longueur — des CARACTÈRES —, et
        // tout index ultérieur sortait des limites (trouvé par le lexeur
        // auto-hébergé de M4 se lexant lui-même, mot de « ≈ » près).
        for (auto &p : f.params) temp_types["%" + p.first] = p.second;
        std::map<std::string, RirType> global_types;
        for (auto &b : f.blocs) {
            for (auto &instr : b.instrs) {
                if (!instr.dest.empty() && instr.dest[0] == '%') {
                    temps.insert(instr.dest);
                    // Type inference for liste.new
                    if (instr.op == "liste.new") temp_types[instr.dest] = RirListe;
                    // Rationnels et opérations exactes
                    if (instr.op == "cst.q" || instr.op == "div.q" || instr.op == "div.z" ||
                        instr.op == "add.q" || instr.op == "sub.q" || instr.op == "mul.q")
                        temp_types[instr.dest] = RirQ;
                    // Index dans une chaîne → élément chaîne
                    if (instr.op == "index" && !instr.args.empty()) {
                        std::string bn = sanitize(instr.args[0].name);
                        RirType bty = RirU;
                        auto bt1 = temp_types.find(instr.args[0].name);
                        if (bt1 != temp_types.end()) bty = bt1->second;
                        auto bt2 = global_types.find(bn);
                        if (bty == RirU && bt2 != global_types.end()) bty = bt2->second;
                        if (bty == RirTbl) temp_types[instr.dest] = RirDyn;   // M4.2
                        else if (bty == RirS) temp_types[instr.dest] = RirS;
                    }
                    // Constante entière hors int64 → grand entier
                    if (instr.op == "cst.z" && !instr.args.empty() &&
                        instr.args[0].kind == RirOp::Const &&
                        cst_promu_gd(instr.args[0].name.rfind("@c", 0) == 0 ?
                                     instr.args[0].name.substr(2) : instr.args[0].name))
                        temp_types[instr.dest] = RirGd;
                }
                // Collect global variables from affect
                // Appels à grands résultats : typer avant l'inférence globale
                if (instr.op == "call" && !instr.dest.empty() && instr.dest[0] == '%' &&
                    !instr.args.empty()) {
                    std::string cn = instr.args[0].name;
                    if (!cn.empty() && cn[0] == '$') cn = cn.substr(1);
                    if (cn == "fraction") temp_types[instr.dest] = RirQ;
                    if (cn == "fibonacci" || cn == "factorielle" || cn == "puissance")
                        temp_types[instr.dest] = RirGd;
                    if (cn == "pgcd" || cn == "ppcm" || cn == "abs" || cn == "min" || cn == "max") {
                        bool gd = false;
                        for (size_t ai = 1; ai < instr.args.size(); ai++) {
                            std::string an = instr.args[ai].name;
                            if (instr.args[ai].kind == RirOp::Const) {
                                std::string cx = an;
                                if (cx.rfind("@c", 0) == 0) cx = cx.substr(2);
                                if (cst_promu_gd(cx)) gd = true;
                                continue;
                            }
                            if (temp_types.count(an) && temp_types[an] == RirGd) gd = true;
                            if (global_types.count(sanitize(an)) && global_types[sanitize(an)] == RirGd) gd = true;
                        }
                        if (gd) temp_types[instr.dest] = RirGd;
                    }
                    // Index sur chaine : le resultat est une chaine d'un caractere.
                    // Miroir de l'emission : toute base non-liste part dans
                    // rose_index_str (rose_str), donc dest = S.
                    if (instr.op == "index" && !instr.dest.empty() && !instr.args.empty()) {
                        std::string bn = instr.args[0].name;
                        bool liste = (temp_types.count(bn) && temp_types[bn] == RirListe) ||
                                     (global_types.count(sanitize(bn)) && global_types[sanitize(bn)] == RirListe);
                        bool tbl = (temp_types.count(bn) && temp_types[bn] == RirTbl) ||
                                   (global_types.count(sanitize(bn)) && global_types[sanitize(bn)] == RirTbl);
                        if (tbl) temp_types[instr.dest] = RirDyn;   // M4.2 : cellule dynamique
                        else if (!liste) temp_types[instr.dest] = RirS;
                    }
                    // Lecture clavier typée : le type cible voyage dans message
                    // (T_ENTIER=1, T_RATIONNEL=2, T_FLOTTANT=3, T_CHAINE=5).
                    if (instr.op == "lire" && !instr.dest.empty()) {
                        int cible = instr.message.empty() ? 1 : atoi(instr.message.c_str());
                        temp_types[instr.dest] =
                            cible == 3 ? RirF : cible == 5 ? RirS : cible == 2 ? RirQ : RirZ;
                    }
                }
                // Propagation flottant pendant la collecte (M4.2 : mixtes
                // Q⊛F, pow a operande flottant) — miroir de l'interprete :
                // un operande flottant fait basculer le resultat en F.
                if (!instr.dest.empty() && instr.dest[0] == '%' && temp_types.find(instr.dest) == temp_types.end() &&
                    (instr.op == "pow" || instr.op.rfind("add.", 0) == 0 || instr.op.rfind("sub.", 0) == 0 ||
                     instr.op.rfind("mul.", 0) == 0 || instr.op.rfind("div.", 0) == 0)) {
                    bool f = false;
                    for (size_t ai = 0; ai < instr.args.size(); ai++) {
                        std::string an = instr.args[ai].name;
                        if (temp_types.count(an) && temp_types[an] == RirF) f = true;
                        if (global_types.count(sanitize(an)) && global_types[sanitize(an)] == RirF) f = true;
                    }
                    if (f) temp_types[instr.dest] = RirF;
                }
                // Propagation grand entier pendant la collecte
                if (!instr.dest.empty() && instr.dest[0] == '%' && temp_types.find(instr.dest) == temp_types.end() &&
                    (instr.op == "add.z" || instr.op == "sub.z" || instr.op == "mul.z" || instr.op == "neg.z" ||
                     instr.op == "pow" || instr.op.rfind("divf", 0) == 0 || instr.op.rfind("rem.z", 0) == 0)) {
                    bool gd = false;
                    // v3.13.1 : un opérande RATIONNEL (div Q⊛grand) interdit la
                    // promotion Gd — rose_div_florentin_q rend un rose_ent (200027).
                    bool qv = false;
                    for (size_t ai = 0; ai < instr.args.size() && !qv; ai++) {
                        const std::string &an2 = instr.args[ai].name;
                        auto tq2 = temp_types.find(an2);
                        if (tq2 != temp_types.end() && tq2->second == RirQ) qv = true;
                        auto gq2 = global_types.find(sanitize(an2));
                        if (gq2 != global_types.end() && gq2->second == RirQ) qv = true;
                    }
                    for (size_t ai = 0; ai < instr.args.size() && !qv; ai++) {
                        std::string an = instr.args[ai].name;
                        if (instr.args[ai].kind == RirOp::Const) {
                            std::string t = an;
                            if (t.rfind("@c", 0) == 0) t = t.substr(2);
                            if (cst_promu_gd(t)) gd = true;
                            continue;
                        }
                        if (temp_types.count(an) && temp_types[an] == RirGd) gd = true;
                        if (global_types.count(sanitize(an)) && global_types[sanitize(an)] == RirGd) gd = true;
                    }
                    if (gd) temp_types[instr.dest] = RirGd;
                }
                if (instr.op == "affect" && !instr.message.empty()) {
                    std::string g = sanitize(instr.message);
                    RirType ty = RirU;
                    // Infer type from the call that produced the source temp
                    if (instr.args.size() > 0 && instr.args[0].kind == RirOp::Temp) {
                        std::string src_temp = instr.args[0].name;
                        for (auto &b2 : f.blocs) for (auto &i2 : b2.instrs) {
                            if (i2.dest == src_temp) {
                                // Check if it's a tampon-returning builtin
                                if (i2.op == "call" && !i2.args.empty()) {
                                    std::string name = i2.args[0].name;
                                    if (name[0] == '$') name = name.substr(1);
                                    if (name == "octets" || name == "tampon") ty = RirTab;
                                    else if (name == "table") ty = RirTbl;
                                    else if (name == "cles" || name == "valeurs") ty = RirListe;
                                    else ty = i2.dest_ty;
                                } else {
                                    ty = i2.dest_ty;
                                }
                                break;
                            }
                        }
                    }
                    if (ty == RirU && instr.args.size() > 0) ty = instr.args[0].ty;
                    // Source GLOBAL : le tampon du checker peut être U/Z alors que
                    // le collecteur connaît le vrai domaine de la variable source
                    // (v3.13.1, graine générative 100011 : $alpha = eta avec eta chaîne).
                    if (instr.args.size() > 0 && instr.args[0].kind == RirOp::Temp &&
                        !temp_types.count(instr.args[0].name)) {
                        auto gs = global_types.find(sanitize(instr.args[0].name));
                        if (gs != global_types.end() && gs->second != RirU &&
                            (ty == RirU || ty == RirZ)) ty = gs->second;
                    }
                    // If still U but assigned from a known tampon, mark as tampon
                    // If source temp is a list, mark global as list
                    if (instr.args.size() > 0 && instr.args[0].kind == RirOp::Temp) {
                        std::string src_temp = instr.args[0].name;
                        if (temp_types.count(src_temp) && temp_types[src_temp] == RirListe) ty = RirListe;
                        if (temp_types.count(src_temp) && temp_types[src_temp] == RirGd) ty = RirGd;
                        if (temp_types.count(src_temp) && temp_types[src_temp] == RirQ) ty = RirQ;
                        if (temp_types.count(src_temp) && temp_types[src_temp] == RirF) ty = RirF;
                        if (ty == RirU && temp_types.count(src_temp) && temp_types[src_temp] == RirZ) ty = RirZ;
                        if (temp_types.count(src_temp) && temp_types[src_temp] == RirS &&
                            (ty == RirU || ty == RirZ)) ty = RirS;
                        // M4.2 : propagation des domaines table/cellule.
                        if (temp_types.count(src_temp) && temp_types[src_temp] == RirTbl) ty = RirTbl;
                        if (temp_types.count(src_temp) && temp_types[src_temp] == RirDyn &&
                            (ty == RirU || ty == RirZ)) ty = RirDyn;
                    }
                    auto rang = [](RirType t) { return t == RirGd ? 5 : t == RirQ ? 4 : t == RirListe ? 3 : t == RirS ? 2 : t == RirF ? 2 : t == RirZ ? 1 : 0; };
                    auto gt = global_types.find(g);
                    if (gt == global_types.end()) global_types[g] = ty;
                    else if (ty != RirU && rang(ty) > rang(gt->second) &&
                             !(gt->second == RirS && ty != RirS && rang(ty) < 3)) gt->second = ty;
                }
            }
        }

        // Declare global variables
        for (auto &kv : global_types) {
            // Un paramètre éclipse le global de même nom (comme l'interprète).
            bool ombre = false;
            for (auto &p : f.params) if (sanitize(p.first) == kv.first) ombre = true;
            if (ombre) continue;
            std::string ct = cpp_type(kv.second);
            if (ct == "rose_val") ct = "rose_ent";
            if (ct == "rose_tampon") out << "    " << ct << " " << kv.first << ";\n";
            else if (ct == "rose_liste") out << "    " << ct << " " << kv.first << ";\n";
            else if (ct == "rose_table" || ct == "rose_dyn") out << "    " << ct << " " << kv.first << ";\n";   // M4.2 : vector/aggregate, pas de « = 0 »
            else if (ct == "rose_str") out << "    " << ct << " " << kv.first << " = \"\";\n";
            else out << "    " << ct << " " << kv.first << " = 0;\n";
        }
        if (!global_types.empty()) out << "\n";

        // v3.14 — dernier écart AOT connu (classe « chemin mort », graines
        // génératives 200051/300071) : une variable n'est CERTAINEMENT
        // initialisée que si elle est affectée dans le préfixe linéaire du
        // bloc d'entrée (avant la première branche). Les autres portent un
        // drapeau « défini » : écriture = pose, lecture = rose_lit (garde).
        // C'est le comportement exact du cœur interprété v3.13.1, sans coût
        // pour les variables certaines (les bancs paient 0 : tout est
        // initialisé en préfixe). Les paramètres sont initialisés par l'appel.
        std::set<std::string> certaines, flagpees;
        {
            bool ligne = true;
            auto est_param = [&](const std::string &s) {
                for (auto &p : f.params) if (sanitize(p.first) == s) return true;
                return false;
            };
            auto est_var = [&](const RirOp &a) -> std::string {
                if (a.kind == RirOp::Temp || a.kind == RirOp::Global) {
                    std::string s = sanitize(a.name);
                    // Les paramètres sont initialisés par l'appel : leur
                    // première lecture, même avant toute affectation, est sûre.
                    if (global_types.count(s) && !est_param(s)) return s;
                }
                return "";
            };
            for (auto &b : f.blocs)
                for (auto &instr : b.instrs) {
                    // v3.14.1 — une lecture ANTÉRIEURE à la première écriture
                    // dans le préfixe rend la variable précaire, même si elle
                    // est écrite plus bas dans ce même préfixe : « $x = x »
                    // ou « $a = $b; $b = 1 » lèvent côté interprète. La
                    // lecture d'une instruction précède toujours son écriture.
                    if (ligne)
                        for (auto &a : instr.args) {
                            std::string v = est_var(a);
                            if (!v.empty() && !certaines.count(v)) flagpees.insert(v);
                        }
                    if (instr.op == "affect" && !instr.message.empty() && ligne)
                        certaines.insert(sanitize(instr.message));
                    if (instr.op == "br" || instr.op == "br.cond") ligne = false;
                }
            for (auto &kv : global_types) {
                bool est_param = false;
                for (auto &p : f.params) if (sanitize(p.first) == kv.first) est_param = true;
                if (!est_param && !certaines.count(kv.first)) flagpees.insert(kv.first);
            }
        }
        // Lecture d'un opérande : garde rose_lit si variable marquée, sinon nom brut.
        auto L = [&](const RirOp &a) -> std::string {
            std::string s = sanitize(a.name);
            if ((a.kind == RirOp::Temp || a.kind == RirOp::Global) && flagpees.count(s))
                return "rose_lit(" + s + ", _def_" + s + ", \"" + nom_visible(a.name) + "\")";
            return s;
        };
        for (auto &g : flagpees) out << "    bool _def_" << g << " = false;\n";
        if (!flagpees.empty()) out << "\n";

        // Declare temp variables
        // Need to determine types from dest_ty
        for (auto &b : f.blocs) {
            for (auto &instr : b.instrs) {
                if (!instr.dest.empty() && instr.dest[0] == '%') {
                    RirType ty = instr.dest_ty;
                    // Miroir de l'émission d'index : toute base non-liste part
                    // dans rose_index_str (rose_str) — l'émetteur note dest_ty=Z
                    // même pour une chaîne, ce qui casserait le g++.
                    if (instr.op == "index" && !instr.args.empty()) {
                        std::string bn = instr.args[0].name;
                        bool liste = (temp_types.count(bn) && temp_types[bn] == RirListe) ||
                                     (global_types.count(sanitize(bn)) && global_types[sanitize(bn)] == RirListe);
                        bool tbl = (temp_types.count(bn) && temp_types[bn] == RirTbl) ||
                                   (global_types.count(sanitize(bn)) && global_types[sanitize(bn)] == RirTbl);
                        if (tbl) ty = RirDyn;              // M4.2 : cellule dynamique
                        else if (!liste) ty = RirS;
                    }
                    // Infer type from call name
                    if (instr.op == "call" && !instr.args.empty()) {
                        std::string name = instr.args[0].name;
                        if (name[0] == '$') name = name.substr(1);
                        if (name == "octets" || name == "tampon") ty = RirTab;
                        else if (name == "table") ty = RirTbl;
                        else if (name == "cles" || name == "valeurs") ty = RirListe;
                        else if (name == "fraction") ty = RirQ;
                        else if (name == "flottant") ty = RirF;
                        else if (name == "fibonacci" || name == "factorielle" || name == "puissance") ty = RirGd;
                        else if (name == "millisecondes" || name == "nb_arguments" || name == "entier" || name == "tampon_case" ||
                                 name == "nb_chiffres" || name == "compter_premiers" ||
                                 name == "pgcd" || name == "ppcm" || name == "hasard" || name == "hasard_systeme" || name == "hasard_vrai" ||
                                 name == "rng_materiel" || name == "rng_mat" || name == "ordre" || name == "temps") ty = RirZ;
                        else if (name == "argument" || name == "chaine" || name == "nombre_pi" ||
                                  name == "lire_texte" || name == "sous_chaine" || name == "caractere" || name == "duree") ty = RirS;
                        else if (name == "partie_entiere") ty = RirZ;
                        else if (name == "est_premier") ty = RirB;
                        else if (name == "abs" || name == "min" || name == "max") {
                            // Check if any arg temp is float (ou rationnel)
                            ty = RirZ;
                            bool a_q = false;
                            for (size_t ai = 1; ai < instr.args.size(); ai++) {
                                if (instr.args[ai].kind == RirOp::Temp) {
                                    for (auto &b2 : f.blocs) for (auto &i2 : b2.instrs) {
                                        if (i2.dest == instr.args[ai].name && i2.dest_ty == RirF) { ty = RirF; break; }
                                        if (i2.dest == instr.args[ai].name && i2.dest_ty == RirQ) a_q = true;
                                    }
                                    if (!a_q && ty != RirF &&
                                        !temp_types.count(instr.args[ai].name)) {
                                        auto gs = global_types.find(sanitize(instr.args[ai].name));
                                        if (gs != global_types.end()) {
                                            if (gs->second == RirF) ty = RirF;
                                            if (gs->second == RirQ) a_q = true;
                                        }
                                    }
                                } else if (instr.args[ai].kind == RirOp::Const) {
                                    std::string cx = instr.args[ai].name;
                                    if (cx.rfind("@c", 0) == 0) cx = cx.substr(2);
                                    if (cx.find('/') != std::string::npos) a_q = true;
                                }
                            }
                            if (ty == RirZ && a_q) ty = RirQ;
                        }
                        // Arguments grands entiers : l'appel vit en grand entier
                        if (name == "pgcd" || name == "ppcm" || name == "abs" ||
                            name == "min" || name == "max") {
                            for (size_t ai = 1; ai < instr.args.size(); ai++) {
                                std::string an = instr.args[ai].name;
                                if (instr.args[ai].kind == RirOp::Const) {
                                    std::string cx = an;
                                    if (cx.rfind("@c", 0) == 0) cx = cx.substr(2);
                                    if (cst_promu_gd(cx)) { ty = RirGd; break; }
                                    continue;
                                }
                                if (temp_types.count(an) && temp_types[an] == RirGd) { ty = RirGd; break; }
                                if (global_types.count(sanitize(an)) && global_types[sanitize(an)] == RirGd) { ty = RirGd; break; }
                            }
                        }
                    }
                    // Opérations arithmétiques : propagent le type grand entier
                    // v3.13.1 : un opérande RATIONNEL interdit la promotion Gd —
                    // « div » en domaine Q émet rose_div_florentin_q (retour rose_ent,
                    // promotion interne), et add/sub/mul avec Q ne passent jamais ici
                    // (ops .q dédiés). Graine générative 200027.
                    bool a_q_arg = false;
                    for (size_t ai = 0; ai < instr.args.size() && !a_q_arg; ai++) {
                        const std::string &an = instr.args[ai].name;
                        auto tq = temp_types.find(an);
                        if (tq != temp_types.end() && tq->second == RirQ) a_q_arg = true;
                        auto gq = global_types.find(sanitize(an));
                        if (gq != global_types.end() && gq->second == RirQ) a_q_arg = true;
                        if (instr.args[ai].kind == RirOp::Const) {
                            std::string t = an;
                            if (t.rfind("@c", 0) == 0) t = t.substr(2);
                            if (t.find('/') != std::string::npos) a_q_arg = true;
                        }
                    }
                    if (!a_q_arg &&
                        (instr.op == "add.z" || instr.op == "sub.z" || instr.op == "mul.z" ||
                         instr.op == "neg.z" || instr.op == "pow" || instr.op == "add" ||
                         instr.op.rfind("divf", 0) == 0 || instr.op.rfind("rem.z", 0) == 0) &&
                        (ty == RirZ || ty == RirU)) {
                        for (size_t ai = 0; ai < instr.args.size(); ai++) {
                            std::string an = instr.args[ai].name;
                            if (instr.args[ai].kind == RirOp::Const) {
                                std::string t = an;
                                if (t.rfind("@c", 0) == 0) t = t.substr(2);
                                if (cst_promu_gd(t)) { ty = RirGd; break; }
                                continue;
                            }
                            if (temp_types.count(an) && temp_types[an] == RirGd) { ty = RirGd; break; }
                            if (global_types.count(sanitize(an)) && global_types[sanitize(an)] == RirGd) { ty = RirGd; break; }
                        }
                    }
                    if (temp_types.find(instr.dest) == temp_types.end() ||
                        (temp_types[instr.dest] != RirListe && temp_types[instr.dest] != RirGd &&
                         temp_types[instr.dest] != RirQ &&
                         !(temp_types[instr.dest] == RirS && ty == RirU)))
                        temp_types[instr.dest] = ty;
                }
            }
        }
        // Type d'un opérande (temp, global ou constante).
        std::map<std::string, RirType> prod_ty;
        for (auto &b : f.blocs) for (auto &i : b.instrs)
            if (!i.dest.empty()) prod_ty[i.dest] = i.dest_ty;
        std::map<std::string, RirType> param_ty;
        for (auto &p : f.params) param_ty[sanitize(p.first)] = p.second;
        // v3.14.1 — une fonction qui RENDE une liste et reçoive un paramètre-liste
        // est ambiguë par partage : l'interprète, via shared_ptr, peut aliaser
        // la liste de l'appelante dans la valeur rendue (les mutations du
        // appelant se reverraient dans l'originale). La copie AOT les
        // perdrait. Rejet explicite tant que le retour n'est pas référencé.
        if (f.retour == RirListe)
            for (auto &p : f.params)
                if (p.second == RirListe)
                    throw std::runtime_error(
                        "AOT : fonction retournant une liste ET prenant un paramètre-liste — "
                        "l'interprète aliasse les deux (partage shared_ptr) ; la copie AOT "
                        "perdrait les mutations (limite M4.2 explicite)");
        auto ty_of = [&](const RirOp &o) -> RirType {
            auto pp = param_ty.find(sanitize(o.name));
            if (pp != param_ty.end() && !o.name.empty() && o.name[0] != '%') return pp->second;
            if (o.kind == RirOp::Const) {   // constante entière promue : vit en gd
                std::string cx = o.name;
                if (cx.rfind("@c", 0) == 0) cx = cx.substr(2);
                if (cst_promu_gd(cx)) return RirGd;
            }
            auto it = temp_types.find(o.name);
            if (it != temp_types.end() && it->second != RirU) return it->second;
            std::string s = sanitize(o.name);
            auto gt = global_types.find(s);
            if (gt != global_types.end() && gt->second != RirU) return gt->second;
            auto pt = prod_ty.find(o.name);
            if (pt != prod_ty.end() && pt->second != RirU) return pt->second;
            return o.ty;
        };
        for (auto &t : temps) {
            RirType ty = temp_types.count(t) ? temp_types[t] : RirU;            std::string ct = cpp_type(ty);
            if (ct == "rose_val") ct = "rose_ent";
            if (ct == "rose_str") out << "    " << ct << " " << sanitize(t) << " = \"\";\n";
            else if (ct == "rose_tampon") out << "    " << ct << " " << sanitize(t) << ";\n";
            else if (ct == "rose_liste") out << "    " << ct << " " << sanitize(t) << ";\n";
            else if (ct == "rose_table" || ct == "rose_dyn") out << "    " << ct << " " << sanitize(t) << ";\n";   // M4.2 : vector/aggregate, pas de « = 0 »
            else out << "    " << ct << " " << sanitize(t) << " = 0;\n";
        }

        // Generate code for each block
        for (auto &b : f.blocs) {
            if (b.nom != ".entree" && !b.nom.empty()) {
                out << "  " << sanitize(b.nom) << ":\n";
            }
            for (auto &instr : b.instrs) {
                const std::string &op = instr.op;
                // v3.14.1 — le marqueur exact du chemin sans valeur : l'émetteur
                // termine les fonctions par « ret.u » (retour sans opérande) ;
                // si la fonction a un retour DÉCLARÉ, ce chemin rend chez
                // l'interprète la valeur vidée — invisible en C++ (le
                // « return 0 » construisait une string depuis NULL et
                // abortait). Rejet explicite à l'instruction.
                if (op == "ret.u" && f.retour != RirU)
                    throw std::runtime_error(
                        "AOT : chemin sans « retour » dans une fonction à retour déclaré — "
                        "l'interprète y rend la valeur vidée, invisible en C++ (limite M4.2 explicite)");
                // ── garde M4.2 : les domaines table/cellule ne franchissent
                // ni l'arithmétique, ni le return, ni les frontières de
                // fonction. Rejet explicite — jamais de code faux silencieux.
                {
                    auto est_dyn = [&](const RirOp &o) {
                        if (o.kind == RirOp::Const) return false;
                        auto t = temp_types.find(o.name);
                        if (t != temp_types.end()) return t->second == RirTbl || t->second == RirDyn;
                        // v3.14.1 — un paramètre-table est dynamique aussi :
                        // l'interprète le partage (shared_ptr), une passe par
                        // valeur en C++ casserait l'aliasing → rejet poli.
                        auto pp = param_ty.find(sanitize(o.name));
                        if (pp != param_ty.end() && o.name[0] != '%') return pp->second == RirTbl;
                        auto g = global_types.find(sanitize(o.name));
                        if (g != global_types.end()) return g->second == RirTbl || g->second == RirDyn;
                        return false;
                    };
                    bool arith = op.rfind("add.", 0) == 0 || op.rfind("sub.", 0) == 0 ||
                                 op.rfind("mul.", 0) == 0 || op.rfind("div.", 0) == 0 ||
                                 op.rfind("divf", 0) == 0 || op.rfind("rem.", 0) == 0 ||
                                 op.rfind("neg.", 0) == 0 || op == "pow" || op.rfind("pow.", 0) == 0;
                    bool appel_user = op == "call" && !instr.args.empty() && fn_rets.count(sanitize(instr.args[0].name)) > 0;
                    if ((arith || op.rfind("ret", 0) == 0 || appel_user)) {
                        for (auto &a : instr.args)
                            if (est_dyn(a))
                                throw std::runtime_error(std::string("AOT : table ou cellule dynamique dans « ") + op +
                                                         " » — l'interprète garde ces valeurs dynamiques (limite M4.2 explicite)");
                    }
                }
                // cst.z
                if (op == "cst.z") {
                    std::string val = "0";
                    if (!instr.args.empty()) {
                        if (instr.args[0].kind == RirOp::Const) {
                            if (instr.args[0].name.rfind("@c", 0) == 0)
                                val = instr.args[0].name.substr(2);
                            else val = instr.args[0].name;
                        }
                    }
                    if (cst_promu_gd(val))
                        out << "    " << sanitize(instr.dest) << " = rose_gd(\"" << val << "\");\n";
                    else
                        out << "    " << sanitize(instr.dest) << " = " << val << ";\n";
                }
                else if (op == "cst.q") {
                    std::string txt;
                    if (!instr.args.empty() && instr.args[0].kind == RirOp::Const) {
                        txt = instr.args[0].name;
                        if (txt.rfind("@c", 0) == 0) txt = txt.substr(2);
                    }
                    out << "    " << sanitize(instr.dest) << " = rose_qd(\"" << txt << "\");\n";
                }
                else if (op == "cst.s") {
                    std::string val = "\"\"";
                    if (!instr.args.empty() && instr.args[0].kind == RirOp::Const) {
                        std::string s = instr.args[0].name;
                        std::string escaped;
                        for (char c : s) {
                            if (c == '"') escaped += "\\\"";
                            else if (c == '\\') escaped += "\\\\";
                            else if (c == '\n') escaped += "\\n";
                            else if (c == '\r') escaped += "\\r";
                            else if (c == '\t') escaped += "\\t";
                            else escaped += c;
                        }
                        val = "\"" + escaped + "\"";
                    }
                    out << "    " << sanitize(instr.dest) << " = " << val << ";\n";
                }
                else if (op == "cst.f") {
                    std::string val = "0.0";
                    if (!instr.args.empty() && instr.args[0].kind == RirOp::Const) {
                        if (instr.args[0].name.rfind("@c", 0) == 0) val = instr.args[0].name.substr(2);
                        else val = instr.args[0].name;
                    }
                    out << "    " << sanitize(instr.dest) << " = " << val << ";\n";
                }
                else if (op == "cst.b") {
                    std::string val = "false";
                    if (!instr.args.empty() && instr.args[0].kind == RirOp::Const) {
                        if (instr.args[0].name == "vrai") val = "true";
                        else if (instr.args[0].name == "faux") val = "false";
                    }
                    out << "    " << sanitize(instr.dest) << " = " << val << ";\n";
                }
                else if (op == "liste.new") {
                    out << "    " << sanitize(instr.dest) << " = rose_liste_new();\n";
                }
                else if (op == "liste.push") {
                    std::string list_var = L(instr.args[0]);
                    std::string elem = L(instr.args[1]);
                    RirType ety = ty_of(instr.args[1]);
                    if (instr.args[1].kind == RirOp::Const && instr.args[1].ty != RirS &&
                        instr.args[1].name.rfind("@c", 0) == 0) ety = RirZ;
                    if (ety == RirS)
                        out << "    rose_liste_push(" << list_var << ", " << elem << ".c_str());\n";
                    else if (ety == RirGd)
                        out << "    rose_liste_push_gd(" << list_var << ", " << elem << ");\n";
                    else if (ety == RirQ)
                        out << "    rose_liste_push_q(" << list_var << ", " << elem << ");\n";
                    else if (ety == RirF)
                        out << "    rose_liste_push_flt(" << list_var << ", " << elem << ");\n";
                    else
                        out << "    rose_liste_push_int(" << list_var << ", " << elem << ");\n";
                }
                else if (op == "index") {
                    std::string dest = sanitize(instr.dest);
                    std::string base = instr.args[0].kind == RirOp::Const
                        ? cpp_operand(instr.args[0], temp_types, &flagpees) : L(instr.args[0]);
                    std::string idx = instr.args[1].kind == RirOp::Const
                        ? cpp_operand(instr.args[1], temp_types, &flagpees) : L(instr.args[1]);
                    RirType bty = temp_types.count(instr.args[0].name) ? temp_types[instr.args[0].name] : RirU;
                    if (global_types.count(base)) bty = global_types[base];
                    if (bty == RirTbl) {   // M4.2 : table → cellule dynamique
                        RirType kty = instr.args[1].kind == RirOp::Const ? RirS : ty_of(instr.args[1]);
                        if (kty != RirS)
                            throw std::runtime_error("AOT : clé de table non textuelle (indexation par nombre — l'interprète compare par valeur)");
                        out << "    " << dest << " = rose_table_get(" << base << ", " << idx << ");\n";
                    }
                    else if (bty == RirListe) {
                        // Liste : élément lu comme chaîne ou entier selon la destination.
                        RirType dty = temp_types.count(instr.dest) ? temp_types[instr.dest] : RirU;
                        if (dty == RirS)
                            out << "    " << dest << " = rose_liste_get_str(" << base << ", " << idx << ");\n";
                        else
                            out << "    " << dest << " = rose_a_entier(rose_liste_get_str(" << base << ", " << idx << "));\n";
                    }
                    else out << "    " << dest << " = rose_index_str(" << base << ", " << idx << ");\n";
                }
                else if (op == "liste.len") {
                    std::string dest = sanitize(instr.dest);
                    std::string base = L(instr.args[0]);
                    out << "    " << dest << " = rose_liste_len(" << base << ");\n";
                }
                else if (op == "affect") {
                    // v3.14.1 — la source passe par cpp_operand AVEC flagpees :
                    // une lecture de variable précaire est gardée par rose_lit
                    // (l'ancienne surcharge brute par sanitize() court-circuitait
                    // la garde « variable utilisée sans valeur »).
                    std::string src_expr = cpp_operand(instr.args[0], temp_types, &flagpees);
                    if (instr.args[0].kind == RirOp::Const) {
                        if (instr.args[0].name.rfind("@c", 0) == 0) src_expr = instr.args[0].name.substr(2);
                        else if (instr.args[0].ty == RirS) {
                            std::string s = instr.args[0].name;
                            std::string escaped;
                            for (char c : s) {
                                if (c == '"') escaped += "\\\"";
                                else if (c == '\\') escaped += "\\\\";
                                else if (c == '\n') escaped += "\\n";
                                else if (c == '\r') escaped += "\\r";
                                else if (c == '\t') escaped += "\\t";
                                else escaped += c;
                            }
                            src_expr = "\"" + escaped + "\"";
                        }
                    }
                    {
                        std::string dglob = sanitize(instr.message);
                        auto gt = global_types.find(dglob);
                        RirType sty = RirU;
                        if (instr.args[0].kind == RirOp::Temp && temp_types.count(instr.args[0].name))
                            sty = temp_types[instr.args[0].name];
                        else if (instr.args[0].kind == RirOp::Global)
                            sty = global_types.count(src_expr) ? global_types[src_expr] : RirU;
                        // Globaux référencés en Temp « %nom » (checker) : résoudre au vrai domaine.
                        if (sty == RirU && instr.args[0].kind == RirOp::Temp) {
                            auto gs = global_types.find(src_expr);
                            if (gs != global_types.end()) sty = gs->second;
                        }
                        // v3.14 — lire une variable possiblement non initialisée
                        // lève l'erreur exactement là où le cœur interprété lève
                        // « variable 'x' utilisée sans valeur » (avant l'écriture).
                        if (instr.args[0].kind == RirOp::Temp || instr.args[0].kind == RirOp::Global) {
                            std::string sn = L(instr.args[0]);
                            if (flagpees.count(sn))
                                out << "    rose_exige(_def_" << sn << ", \"" << nom_visible(instr.args[0].name) << "\");\n";
                        }
                        if (gt != global_types.end() && gt->second == RirF && sty == RirZ)
                            out << "    " << dglob << " = rose_vers_double(" << src_expr << ");\n";
                        else if (gt != global_types.end() && gt->second == RirGd && (sty == RirZ || sty == RirU))
                            out << "    " << dglob << " = rose_vers_mpz(" << src_expr << ");\n";
                        else if (gt != global_types.end() && gt->second == RirQ && (sty == RirZ || sty == RirU))
                            // entier → rationnel : élargissement légal, promotion mpq (v3.13.1)
                            out << "    " << dglob << " = rose_q_de(" << src_expr << ");\n";
                        else if (gt != global_types.end() && sty != RirU && sty != gt->second &&
                                 ((gt->second == RirZ && (sty == RirS || sty == RirB || sty == RirQ || sty == RirF)) ||
                                  (gt->second == RirGd && (sty == RirS || sty == RirB || sty == RirQ || sty == RirF)) ||
                                  (gt->second == RirS) ||
                                  (gt->second == RirB) ||
                                  (gt->second == RirQ && (sty == RirF || sty == RirS || sty == RirB)) ||
                                  (gt->second == RirF && (sty == RirQ || sty == RirS || sty == RirB))))
                            // Le checker laisse passer certaines affectations hétérogènes
                            // que l'interprète, dynamique, absorbe. L'AOT statique ne peut
                            // pas : rejet poli, jamais un g++ cassé (contrat M4.2, v3.13.1).
                            throw std::runtime_error("AOT : la variable « " + instr.message +
                                    " » change de domaine — l'interprète la garde dynamique (limite explicite)");
                        else if (gt != global_types.end() && gt->second == RirDyn && sty != RirDyn)
                            // M4.2 : emplacement cellule ← valeur typée : élévation.
                            out << "    " << dglob << " = rose_dyn_de("
                                << (instr.args[0].kind == RirOp::Const && (sty == RirZ || sty == RirU)
                                        ? "rose_ent(" + src_expr + ")" : src_expr) << ");\n";
                        else if (sty == RirDyn && gt != global_types.end() && gt->second != RirDyn)
                            throw std::runtime_error("AOT : valeur de table recopiée dans une variable typée — l'interprète la garde dynamique (limite M4.2 explicite)");
                        else
                            out << "    " << dglob << " = " << src_expr << ";\n";
                        // v3.14 — écrire définit la variable (drapeau pour les
                        // lectures gardées ; aucune dépense pour les autres).
                        if (flagpees.count(dglob))
                            out << "    _def_" << dglob << " = true;\n";
                        // v3.14.1 — une variable qui reçoit une liste en hérite
                        // le type statique sous la clé « %nom » : les lectures
                        // (index, longueur, test dynamique) consultent cette clé,
                        // sans quoi un index sur liste locale émettrait
                        // rose_index_str (chaîne) et casserait g++.
                        if (sty == RirListe)
                            temp_types["%" + instr.message] = RirListe;
                    }
                }
                else if (op == "afficher") {
                    std::string arg = L(instr.args[0]);
                    RirType ty = RirU;
                    if (temp_types.count(instr.args[0].name)) ty = temp_types[instr.args[0].name];
                    else if (global_types.count(sanitize(instr.args[0].name))) ty = global_types[sanitize(instr.args[0].name)];
                    else ty = instr.args[0].ty;
                    if (ty == RirZ) out << "    rose_afficher_int(" << arg << ");\n";
                    else if (ty == RirQ) out << "    rose_afficher_q(" << arg << ");\n";
                    else if (ty == RirGd) out << "    rose_afficher_gd(" << arg << ");\n";
                    else if (ty == RirF) out << "    rose_afficher_float(" << arg << ");\n";
                    else if (ty == RirB) out << "    rose_afficher_bool(" << arg << ");\n";
                    else if (ty == RirS) out << "    rose_afficher_str(" << arg << ".c_str());\n";
                    else if (ty == RirTab) out << "    rose_afficher_tampon_longueur(" << arg << ");\n";
                    else if (ty == RirTbl) out << "    rose_afficher_table(" << arg << ");\n";   // M4.2
                    else if (ty == RirDyn) out << "    " << arg << ".afficher();\n";             // M4.2 (miroir ecri)
                    else if (ty == RirListe) out << "    rose_afficher_liste(" << arg << ");\n";
                    else out << "    rose_afficher_int(" << arg << ");\n";
                }
                else if (op == "lire") {
                    std::string dest = sanitize(instr.dest);
                    RirType lty = temp_types.count(instr.dest) ? temp_types[instr.dest] : RirZ;
                    if (lty == RirF) out << "    " << dest << " = rose_lire_flottant();\n";
                    else if (lty == RirS) out << "    " << dest << " = rose_lire_chaine();\n";
                    else if (lty == RirQ) out << "    " << dest << " = rose_lire_rationnel();\n";
                    else out << "    " << dest << " = rose_lire_int();\n";
                }
                else if (op == "call") {
                    // builtin call: $name args...
                    if (instr.args.empty()) continue;
                    std::string name = instr.args[0].name;
                    if (name[0] == '$') name = name.substr(1);
                    std::string sname = sanitize(name);
                    // Appel de fonction utilisateur
                    if (user_fns.count(sname)) {
                        std::string call = sname + "(";
                        const std::vector<RirType> &pt = fn_ptys[sname];
                        for (size_t ai = 1; ai < instr.args.size(); ai++) {
                            if (ai > 1) call += ", ";
                            std::string argx = cpp_operand(instr.args[ai], temp_types);
                            // v3.14 — lecture gardée des variables marquées ; la
                            // conversion de paramètre (Gd/F/Z) s'applique par-dessus,
                            // donc la garde l'opérande avant toute arithmétique.
                            { std::string gv = (instr.args[ai].kind == RirOp::Temp ||
                                                instr.args[ai].kind == RirOp::Global)
                                               ? sanitize(instr.args[ai].name) : "";
                              if (flagpees.count(gv))
                                  argx = "rose_lit(" + gv + ", _def_" + gv + ", \"" +
                                         nom_visible(instr.args[ai].name) + "\")"; }
                            size_t pi = ai - 1;
                            if (pi < pt.size()) {
                                RirType aty = RirU;
                                if (instr.args[ai].kind == RirOp::Temp && temp_types.count(instr.args[ai].name))
                                    aty = temp_types[instr.args[ai].name];
                                else if (instr.args[ai].kind == RirOp::Const) aty = instr.args[ai].ty;
                                else if (instr.args[ai].kind == RirOp::Global && global_types.count(argx))
                                    aty = global_types[argx];
                                if (pt[pi] == RirGd && (aty == RirZ || aty == RirU) &&
                                    instr.args[ai].kind != RirOp::Const)
                                    argx = "rose_vers_mpz(" + argx + ")";
                                if (pt[pi] == RirZ && aty == RirF)
                                    argx = "rose_depuis_double(" + argx + ")";
                                if (pt[pi] == RirF && (aty == RirZ || aty == RirU) &&
                                    instr.args[ai].kind != RirOp::Const)
                                    argx = "rose_vers_double(" + argx + ")";
                            }
                            call += argx;
                        }
                        call += ")";
                        if (!instr.dest.empty()) {
                            RirType dty = RirU;
                            if (instr.dest[0] == '%' && temp_types.count(instr.dest)) dty = temp_types[instr.dest];
                            else if (global_types.count(sanitize(instr.dest))) dty = global_types[sanitize(instr.dest)];
                            RirType rty = RirU;
                            auto frt = fn_ptys.count(sname) ? fn_rets.find(sname) : fn_rets.end();
                            if (frt != fn_rets.end()) rty = frt->second;
                            if (dty == RirGd && (rty == RirZ || rty == RirU))
                                out << "    " << sanitize(instr.dest) << " = rose_vers_mpz(" << call << ");\n";
                            else if (dty == RirZ && rty == RirF)
                                out << "    " << sanitize(instr.dest) << " = rose_depuis_double(" << call << ");\n";
                            else if (dty == RirF && (rty == RirZ || rty == RirU))
                                out << "    " << sanitize(instr.dest) << " = rose_vers_double(" << call << ");\n";
                            else
                                out << "    " << sanitize(instr.dest) << " = " << call << ";\n";
                        }
                        else
                            out << "    " << call << ";\n";
                    }
                    // Map to runtime function
                    else if (name == "octets") {
                        std::string arg = cpp_operand(instr.args[1], temp_types, &flagpees);
                        if (instr.args[1].kind == RirOp::Temp) arg = L(instr.args[1]) + ".c_str()";
                        if (instr.args[1].kind == RirOp::Const) {
                            std::string s = instr.args[1].name;
                            std::string escaped;
                            for (char c : s) {
                                if (c == '"') escaped += "\\\"";
                                else if (c == '\\') escaped += "\\\\";
                                else escaped += c;
                            }
                            arg = "\"" + escaped + "\"";
                        }
                        out << "    " << sanitize(instr.dest) << " = rose_octets(" << arg << ");\n";
                    }
                    else if (name == "tampon") {
                        std::string arg = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_tampon_new(" << arg << ");\n";
                    }
                    else if (name == "graine") {
                        std::string arg = L(instr.args[1]);
                        out << "    rose_graine(" << arg << ");\n";
                    }
                    else if (name == "graine_systeme" || name == "graine_vrai") {
                        out << "    rose_graine_systeme();\n";
                    }
                    else if (name == "rng_materiel" || name == "rng_mat") {
                        out << "    " << sanitize(instr.dest) << " = rose_rng_materiel();\n";
                    }
                    else if (name == "remplir_hasard") {
                        std::string a0 = L(instr.args[1]);
                        std::string a1 = L(instr.args[2]);
                        out << "    rose_remplir_hasard(" << a0 << ", " << a1 << ");\n";
                    }
                    else if (name == "millisecondes") {
                        out << "    " << sanitize(instr.dest) << " = rose_millisecondes();\n";
                    }
                    else if (name == "temps") {
                        out << "    " << sanitize(instr.dest) << " = rose_temps();\n";
                    }
                    else if (name == "nb_arguments") {
                        out << "    " << sanitize(instr.dest) << " = rose_nb_arguments();\n";
                    }
                    else if (name == "argument") {
                        std::string arg = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_argument(" << arg << ");\n";
                    }
                    else if (name == "entier") {
                        std::string arg = L(instr.args[1]);
                        RirType aty = ty_of(instr.args[1]);
                        if (instr.args[1].kind == RirOp::Const && instr.args[1].name.rfind("@c", 0) == 0)
                            out << "    " << sanitize(instr.dest) << " = " << instr.args[1].name.substr(2) << ";\n";
                        else if (aty == RirF)
                            out << "    " << sanitize(instr.dest) << " = rose_entier_f(" << arg << ");\n";
                        else if (aty == RirQ)
                            out << "    " << sanitize(instr.dest) << " = rose_entier_q(" << arg << ");\n";
                        else if (aty == RirS || aty == RirU)
                            out << "    " << sanitize(instr.dest) << " = rose_entier(" << arg << ".c_str());\n";
                        else
                            out << "    " << sanitize(instr.dest) << " = " << arg << ";\n";
                    }
                    else if (name == "flottant") {
                        // miroir de l'interprète (rose.cpp 3558) : chaine →
                        // chaine_vers_flottant, sinon vers_d (B→1.0/0.0,
                        // Z→mpz_get_d, Q→mpq_get_d, F→identité).
                        std::string arg = L(instr.args[1]);
                        RirType aty = ty_of(instr.args[1]);
                        if (instr.args[1].kind == RirOp::Const && instr.args[1].name.rfind("@c", 0) == 0)
                            out << "    " << sanitize(instr.dest) << " = " << instr.args[1].name.substr(2) << ";\n";
                        else if (aty == RirS || aty == RirU)
                            out << "    " << sanitize(instr.dest) << " = rose_flottant_chaine(" << arg << ");\n";
                        else if (aty == RirB)
                            out << "    " << sanitize(instr.dest) << " = (" << arg << " ? 1.0 : 0.0);\n";
                        else if (aty == RirF)
                            out << "    " << sanitize(instr.dest) << " = " << arg << ";\n";
                        else
                            out << "    " << sanitize(instr.dest) << " = rose_vers_double(" << arg << ");\n";
                    }
                    else if (name == "pgcd") {
                        std::string a0 = L(instr.args[1]);
                        std::string a1 = L(instr.args[2]);
                        if (ty_of(instr.args[1]) == RirGd || ty_of(instr.args[2]) == RirGd)
                            out << "    " << sanitize(instr.dest) << " = rose_pgcd_gd(" << a0 << ", " << a1 << ");\n";
                        else
                            out << "    " << sanitize(instr.dest) << " = rose_pgcd(" << a0 << ", " << a1 << ");\n";
                    }
                    else if (name == "ppcm") {
                        std::string a0 = L(instr.args[1]);
                        std::string a1 = L(instr.args[2]);
                        if (ty_of(instr.args[1]) == RirGd || ty_of(instr.args[2]) == RirGd)
                            out << "    " << sanitize(instr.dest) << " = rose_ppcm_gd(" << a0 << ", " << a1 << ");\n";
                        else
                            out << "    " << sanitize(instr.dest) << " = rose_ppcm(" << a0 << ", " << a1 << ");\n";
                    }
                    else if (name == "factorielle") {
                        std::string a0 = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_factorielle_gd(" << a0 << ");\n";
                    }
                    else if (name == "fibonacci") {
                        std::string a0 = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_fibonacci_gd(" << a0 << ");\n";
                    }
                    else if (name == "puissance") {
                        std::string a0 = L(instr.args[1]);
                        std::string a1 = L(instr.args[2]);
                        out << "    " << sanitize(instr.dest) << " = rose_puissance_gd(" << a0 << ", " << a1 << ");\n";
                    }
                    else if (name == "est_premier") {
                        std::string a0 = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_est_premier(" << a0 << ");\n";
                    }
                    else if (name == "compter_premiers") {
                        std::string a0 = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_compter_premiers(" << a0 << ");\n";
                    }
                    else if (name == "nombre_pi") {
                        std::string a0 = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_nombre_pi(" << a0 << ");\n";
                    }
                    else if (name == "partie_entiere") {
                        std::string a0 = L(instr.args[1]);
                        RirType aty = ty_of(instr.args[1]);
                        if (aty == RirF)
                            out << "    " << sanitize(instr.dest) << " = rose_partie_entiere_f(" << a0 << ");\n";
                        else if (aty == RirQ)
                            out << "    " << sanitize(instr.dest) << " = rose_partie_entiere_q(" << a0 << ");\n";
                        else
                            out << "    " << sanitize(instr.dest) << " = " << a0 << ";\n";
                    }
                    else if (name == "nb_chiffres") {
                        std::string a0 = L(instr.args[1]);
                        RirType aty = ty_of(instr.args[1]);
                        if (aty == RirGd)
                            out << "    " << sanitize(instr.dest) << " = rose_nb_chiffres_gd(" << a0 << ");\n";
                        else
                            out << "    " << sanitize(instr.dest) << " = rose_nb_chiffres(" << a0 << ");\n";
                    }
                    else if (name == "hasard") {
                        std::string a0 = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_hasard(" << a0 << ");\n";
                    }
                    else if (name == "hasard_systeme" || name == "hasard_vrai") {
                        std::string a0 = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_hasard_systeme(" << a0 << ");\n";
                    }
                    else if (name == "abs") {
                        std::string a0 = L(instr.args[1]);
                        RirType aty = ty_of(instr.args[1]);
                        if (aty == RirF) out << "    " << sanitize(instr.dest) << " = rose_abs_f(" << a0 << ");\n";
                        else if (aty == RirGd) out << "    " << sanitize(instr.dest) << " = rose_abs_gd(" << a0 << ");\n";
                        else if (aty == RirQ) out << "    " << sanitize(instr.dest) << " = rose_abs_q(" << a0 << ");\n";
                        else out << "    " << sanitize(instr.dest) << " = rose_abs(" << a0 << ");\n";
                    }
                    else if (name == "min") {
                        std::string a0 = L(instr.args[1]);
                        std::string a1 = L(instr.args[2]);
                        RirType t0 = ty_of(instr.args[1]), t1 = ty_of(instr.args[2]);
                        if (t0 == RirF || t1 == RirF) {
                            // Spécification cœur : promotion au domaine dominant
                            // puis c <= 0 ? x : y — l'égalité ET NaN renvoient le
                            // PREMIER opérande (rose_min_f « a < b ? a : b »
                            // renvoyait le second sur NaN — miroir strict v3.14,
                            // Q/Gd/Z descendent en double, jamais l'inverse).
                            std::string x0 = t0 == RirF ? a0 : "rose_vers_double(" + a0 + ")";
                            std::string x1 = t1 == RirF ? a1 : "rose_vers_double(" + a1 + ")";
                            out << "    " << sanitize(instr.dest) << " = (" << x0 << " > " << x1 << " ? " << x1 << " : " << x0 << ");\n";
                        }
                        else if (t0 == RirQ || t1 == RirQ)
                            out << "    " << sanitize(instr.dest) << " = rose_min_q(" << a0 << ", " << a1 << ");\n";
                        else if (t0 == RirGd || t1 == RirGd)
                            out << "    " << sanitize(instr.dest) << " = rose_min_gd(" << a0 << ", " << a1 << ");\n";
                        else out << "    " << sanitize(instr.dest) << " = rose_min(" << a0 << ", " << a1 << ");\n";
                    }
                    else if (name == "max") {
                        std::string a0 = L(instr.args[1]);
                        std::string a1 = L(instr.args[2]);
                        RirType t0 = ty_of(instr.args[1]), t1 = ty_of(instr.args[2]);
                        if (t0 == RirF || t1 == RirF) {
                            // Miroir strict de l'interprète (c >= 0 ? x : y) :
                            // égalité et NaN renvoient le premier opérande.
                            std::string x0 = t0 == RirF ? a0 : "rose_vers_double(" + a0 + ")";
                            std::string x1 = t1 == RirF ? a1 : "rose_vers_double(" + a1 + ")";
                            out << "    " << sanitize(instr.dest) << " = (" << x0 << " < " << x1 << " ? " << x1 << " : " << x0 << ");\n";
                        }
                        else if (t0 == RirQ || t1 == RirQ)
                            out << "    " << sanitize(instr.dest) << " = rose_max_q(" << a0 << ", " << a1 << ");\n";
                        else if (t0 == RirGd || t1 == RirGd)
                            out << "    " << sanitize(instr.dest) << " = rose_max_gd(" << a0 << ", " << a1 << ");\n";
                        else out << "    " << sanitize(instr.dest) << " = rose_max(" << a0 << ", " << a1 << ");\n";
                    }
                    else if (name == "fraction") {
                        std::string a0 = cpp_operand(instr.args[1], temp_types, &flagpees);
                        std::string a1 = cpp_operand(instr.args[2], temp_types, &flagpees);
                        out << "    " << sanitize(instr.dest) << " = rose_qd(rose_q_de(" << a0 << ") / rose_q_de(" << a1 << "));\n";
                    }
                    else if (name == "tampon_case") {
                        std::string a0 = L(instr.args[1]);
                        std::string a1 = L(instr.args[2]);
                        out << "    " << sanitize(instr.dest) << " = rose_tampon_case(" << a0 << ", " << a1 << ");\n";
                    }
                    else if (name == "tampon_pose") {
                        std::string a0 = L(instr.args[1]);
                        std::string a1 = L(instr.args[2]);
                        std::string a2 = L(instr.args[3]);
                        out << "    rose_tampon_pose(" << a0 << ", " << a1 << ", " << a2 << ");\n";
                    }
                    else if (name == "joindre") {
                        // joindre(list, elem): push elem to list
                        std::string list_var = L(instr.args[1]);
                        std::string elem = L(instr.args[2]);
                        if (instr.args[2].kind == RirOp::Const) {
                            std::string s = instr.args[2].name;
                            if (s.rfind("@c", 0) == 0) {
                                out << "    rose_liste_push_int(" << list_var << ", " << s.substr(2) << ");\n";
                                continue;
                            }
                            std::string escaped;
                            for (char c : s) {
                                if (c == '"') escaped += "\\\"";
                                else if (c == '\\') escaped += "\\\\";
                                else escaped += c;
                            }
                            out << "    rose_liste_push(" << list_var << ", \"" << escaped << "\".c_str());\n";
                        } else {
                            // elem est un temp : pousser selon son type.
                            RirType ety = ty_of(instr.args[2]);
                            if (ety == RirGd)
                                out << "    rose_liste_push_gd(" << list_var << ", " << elem << ");\n";
                            else if (ety == RirQ)
                                out << "    rose_liste_push_q(" << list_var << ", " << elem << ");\n";
                            else if (ety == RirF)
                                out << "    rose_liste_push_flt(" << list_var << ", " << elem << ");\n";
                            else if (ety == RirZ)
                                out << "    rose_liste_push_int(" << list_var << ", " << elem << ");\n";
                            else
                                out << "    rose_liste_push(" << list_var << ", " << elem << ".c_str());\n";
                        }
                    }
                    else if (name == "concatener") {
                        std::string list_var = L(instr.args[1]);
                        out << "    " << sanitize(instr.dest) << " = rose_liste_concat(" << list_var << ");\n";
                    }
                    else if (name == "longueur") {
                        std::string arg = sanitize(instr.args[1].name);   // clé de résolution de domaine
                        RirType aty = RirU;
                        if (temp_types.count(instr.args[1].name)) aty = temp_types[instr.args[1].name];
                        else if (global_types.count(arg)) aty = global_types[arg];
                        else aty = instr.args[1].ty;
                        // v3.14 — l'expression émise passe par la garde (arg, lui, sert de clé).
                        std::string argE = L(instr.args[1]);
                        if (aty == RirListe) out << "    " << sanitize(instr.dest) << " = rose_liste_len(" << argE << ");\n";
                        else if (aty == RirTbl) out << "    " << sanitize(instr.dest) << " = rose_table_longueur(" << argE << ");\n";   // M4.2
                        else if (aty == RirTab) out << "    " << sanitize(instr.dest) << " = rose_tampon_longueur(" << argE << ");\n";
                        else if (aty == RirS)
                            out << "    " << sanitize(instr.dest) << " = rose_longueur(" << argE << ");\n";
                        else out << "    " << sanitize(instr.dest) << " = " << argE << ".size();\n";
                    }
                    else if (name == "ordre") {
                        out << "    " << sanitize(instr.dest) << " = rose_ordre("
                            << cpp_operand(instr.args[1], temp_types, &flagpees) << ");\n";
                    }
                    else if (name == "sous_chaine") {
                        out << "    " << sanitize(instr.dest) << " = rose_sous_chaine("
                            << cpp_operand(instr.args[1], temp_types, &flagpees) << ", "
                            << cpp_operand(instr.args[2], temp_types, &flagpees) << ", "
                            << cpp_operand(instr.args[3], temp_types, &flagpees) << ");\n";
                    }
                    else if (name == "caractere") {
                        out << "    " << sanitize(instr.dest) << " = rose_caractere("
                            << cpp_operand(instr.args[1], temp_types, &flagpees) << ");\n";
                    }
                    else if (name == "duree") {
                        out << "    " << sanitize(instr.dest) << " = rose_duree("
                            << cpp_operand(instr.args[1], temp_types, &flagpees) << ");\n";
                    }
                    else if (name == "lire_texte") {
                        out << "    " << sanitize(instr.dest) << " = rose_lire_texte("
                            << cpp_operand(instr.args[1], temp_types, &flagpees) << ");\n";
                    }
                    else if (name == "chaine") {
                        std::string arg = L(instr.args[1]);
                        RirType aty = ty_of(instr.args[1]);
                        if (aty == RirQ)
                            // chaine(rationnel) = miroir chaine_de : « a/b »
                            // (rose_texte_q, lui, est le rendu DECIMAL de
                            // ecri — employé par afficher et les listes).
                            out << "    " << sanitize(instr.dest) << " = rose_texte_q_ab(" << arg << ");\n";
                        else if (aty == RirF)
                            out << "    " << sanitize(instr.dest) << " = rose_texte_flt(" << arg << ");\n";
                        else if (aty == RirGd)
                            out << "    " << sanitize(instr.dest) << " = std::string(" << arg << ".get_str().c_str());\n";
                        else if (aty == RirZ)
                            out << "    " << sanitize(instr.dest) << " = rose_chaine(" << arg << ");\n";
                        else if (aty == RirS)
                            // chaine(chaine) = identité (miroir de l'interprète, v3.14).
                            out << "    " << sanitize(instr.dest) << " = " << arg << ";\n";
                        else if (aty == RirB)
                            // chaine(booleen) = « vrai »/« faux » (rose_chaine sur
                            // bool lèverait une ambiguïté d'surcharges — v3.14).
                            out << "    " << sanitize(instr.dest) << " = (" << arg
                                << " ? \"vrai\" : \"faux\");\n";
                        else
                            out << "    " << sanitize(instr.dest) << " = rose_chaine(" << arg << ");\n";
                    }
                    else if (name == "quitter" && instr.args.size() > 1) {
                        out << "    rose_quitter(" << cpp_operand(instr.args[1], temp_types, &flagpees) << ");\n";
                    }
                    else if (name == "ecrire_texte" && instr.args.size() > 2) {
                        out << "    rose_ecrire_texte(" << cpp_operand(instr.args[1], temp_types, &flagpees)
                            << ", " << cpp_operand(instr.args[2], temp_types, &flagpees) << ");\n";
                    }
                    // ── tables (M4.2) — miroirs exacts du runtime ────────
                    else if (name == "table") {
                        out << "    " << sanitize(instr.dest) << " = rose_table_new();\n";
                    }
                    else if (name == "inserer" && instr.args.size() > 3) {
                        RirType kty = ty_of(instr.args[2]);
                        if (kty != RirS)
                            throw std::runtime_error("AOT : clé de table non textuelle — l'interprète compare par valeur, le texte collisionnerait (« 5 » vs 5)");
                        std::string t = L(instr.args[1]);
                        std::string k = cpp_operand(instr.args[2], temp_types, &flagpees);
                        std::string v = cpp_operand(instr.args[3], temp_types, &flagpees);
                        out << "    rose_table_inserer(" << t << ", " << k << ", rose_dyn_de(" << v << "));\n";
                    }
                    else if (name == "cle_existe" && instr.args.size() > 2) {
                        RirType kty = ty_of(instr.args[2]);
                        if (kty != RirS)
                            throw std::runtime_error("AOT : clé de table non textuelle — l'interprète compare par valeur, le texte collisionnerait (« 5 » vs 5)");
                        out << "    " << sanitize(instr.dest) << " = rose_table_cle_existe(" << L(instr.args[1])
                            << ", " << cpp_operand(instr.args[2], temp_types, &flagpees) << ");\n";
                    }
                    else if (name == "supprimer" && instr.args.size() > 2) {
                        RirType kty = ty_of(instr.args[2]);
                        if (kty != RirS)
                            throw std::runtime_error("AOT : clé de table non textuelle — l'interprète compare par valeur, le texte collisionnerait (« 5 » vs 5)");
                        out << "    " << sanitize(instr.dest) << " = rose_table_supprimer(" << L(instr.args[1])
                            << ", " << cpp_operand(instr.args[2], temp_types, &flagpees) << ");\n";
                    }
                    else if (name == "cles" && instr.args.size() > 1) {
                        out << "    " << sanitize(instr.dest) << " = rose_table_cles(" << L(instr.args[1]) << ");\n";
                    }
                    else if (name == "valeurs" && instr.args.size() > 1) {
                        out << "    " << sanitize(instr.dest) << " = rose_table_valeurs(" << L(instr.args[1]) << ");\n";
                    }
                    else {
                        // M4.2 contrat : JAMAIS de trou silencieux — un appel
                        // non transpilé est un rejet explicite (le catch
                        // std::exception du build l'affiche en
                        // « Rose : erreur — ... » avec code 1).
                        throw std::runtime_error("AOT : fonction « " + name + " » non transpilée");
                    }
                }
                else if (op == "br") {
                    if (!instr.label_cible.empty()) {
                        out << "    goto " << sanitize(instr.label_cible) << ";\n";
                    }
                }
                else if (op == "br.cond") {
                    std::string cond = L(instr.args[0]);
                    out << "    if (" << cond << ") goto " << sanitize(instr.label_cible) << ";\n";
                    out << "    else goto " << sanitize(instr.label_cible2) << ";\n";
                }
                else if (op == "not.b") {
                    out << "    " << sanitize(instr.dest) << " = !" << L(instr.args[0]) << ";\n";
                }
                else if (op == "and.b") {
                    out << "    " << sanitize(instr.dest) << " = (" << L(instr.args[0])
                        << " && " << L(instr.args[1]) << ");\n";
                }
                else if (op == "or.b") {
                    out << "    " << sanitize(instr.dest) << " = (" << L(instr.args[0])
                        << " || " << L(instr.args[1]) << ");\n";
                }
                else if (op.rfind("cmp.", 0) == 0) {
                    // cmp.lt.z %t4 %k %t3 → t4 = (k < t3)
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    RirType t0 = ty_of(instr.args[0]), t1 = ty_of(instr.args[1]);
                    bool q0 = (t0 == RirQ), q1 = (t1 == RirQ);
                    // Approximation ≈ / ?= / =~ : tolérance 1e-9 relative (miroir).
                    if (op == "cmp.approx") {
                        bool fla = (t0 == RirF || t1 == RirF);
                        if (fla) {
                            // Spécification cœur (InterpRir cmp.approx) : un
                            // opérande FLOTTANT rend la comparaison DOUBLE pure
                            // (tolérance 1e-9 relative — rose_approx_f porte la
                            // même formule). Convertir le FLOTTEUR en mpq lèverait
                            // SIGFPE sur ±inf/NaN (mpq_set_d) : le Q/Gd d'en face
                            // descend en double, jamais l'inverse. v3.14, même
                            // miroir que cmp_* « flottant dominant ».
                            std::string x0 = a0, x1 = a1;
                            if (t0 != RirF) x0 = "rose_vers_double(" + a0 + ")";
                            if (t1 != RirF) x1 = "rose_vers_double(" + a1 + ")";
                            out << "    " << dest << " = rose_approx_f(" << x0 << ", " << x1 << ");\n";
                        }
                        else if (q0 || q1)
                            out << "    " << dest << " = rose_approx_q(rose_q_de(" << a0 << "), rose_q_de(" << a1 << "), 0);\n";
                        else
                            out << "    " << dest << " = (" << a0 << " == " << a1 << ");\n";
                        continue;
                    }
                    // Comparison operator
                    std::string cmpop = "<";
                    if (op.find(".lt.") != std::string::npos) cmpop = "<";
                    else if (op.find(".le.") != std::string::npos) cmpop = "<=";
                    else if (op.find(".gt.") != std::string::npos) cmpop = ">";
                    else if (op.find(".ge.") != std::string::npos) cmpop = ">=";
                    else if (op.find(".eq.") != std::string::npos) cmpop = "==";
                    else if (op.find(".ne.") != std::string::npos) cmpop = "!=";
                    if (t0 == RirTbl || t1 == RirTbl) {
                        // Table : seule l'égalité est miroitable (l'interprete
                        // compare les tables par paires ; table vs scalaire →
                        // faux par ty differents). Le reste = rejet explicite.
                        if (cmpop != "==" && cmpop != "!=")
                            throw std::runtime_error("AOT : ordre (<, ≤, >, ≥) sur une table non transponible");
                        if (t0 != t1)
                            out << "    " << dest << " = " << (cmpop == "==" ? "false" : "true") << ";\n";
                        else
                            out << "    " << dest << " = (rose_table_eq(" << a0 << ", " << a1 << ") " << cmpop << " true);\n";
                    }
                    else if (t0 == RirDyn || t1 == RirDyn) {
                        // Cellule dynamique : eq/ne = miroir rose_valeur_egale
                        // (texte et booleen d'abord, numerique ensuite) ;
                        // ordre = miroir cmp_vals (F vers_d, Q mpq exact, mpz).
                        if (cmpop == "==" || cmpop == "!=")
                            out << "    " << dest << " = (rose_dyn_eq(" << a0 << ", " << a1 << ") " << cmpop << " true);\n";
                        else
                            out << "    " << dest << " = (rose_dyn_cmp(" << a0 << ", " << a1 << ") " << cmpop << " 0);\n";
                    }
                    else if ((q0 || q1) && (t0 == RirF || t1 == RirF)) {
                        // Spécification cmp_vals (cœur interprété) : dès qu'un
                        // opérande est FLOTTANT, la comparaison se fait en double
                        // pur (vers_d des deux côtés). Convertir le FLOTTEUR en
                        // mpq exploserait en SIGFPE (mpq_set_d) sur NaN/Inf —
                        // graine générative 20000000087 : delta = (−73.7)^434281
                        // = ±inf, inf·0 = NaN. Ici, c'est l'entier/rationnel qui
                        // passe en double, jamais l'inverse.
                        std::string x0 = a0, x1 = a1;
                        if (t0 != RirF) x0 = "rose_vers_double(" + a0 + ")";
                        if (t1 != RirF) x1 = "rose_vers_double(" + a1 + ")";
                        out << "    " << dest << " = (" << x0 << " " << cmpop << " " << x1 << ");\n";
                    }
                    else if (q0 || q1) {
                        // Comparaison exacte rationnel vs entier/flottant.
                        std::string call = "rose_q_cmp(rose_q_de(" + a0 + "), rose_q_de(" + a1 + ")) " +
                                          (cmpop == "<" ? "< 0" : cmpop == "<=" ? "<= 0" :
                                           cmpop == ">" ? "> 0" : cmpop == ">=" ? ">= 0" :
                                           cmpop == "==" ? "== 0" : "!= 0");
                        out << "    " << dest << " = (" << call << ");\n";
                    } else if ((t0 == RirTab || t1 == RirTab) && (cmpop == "==" || cmpop == "!=")) {
                        // Tampons : comparaison par contenu (pas par pointeur).
                        out << "    " << dest << " = (*" << a0 << " " << cmpop << " *" << a1 << ");\n";
                    } else {
                        std::string x0 = a0, x1 = a1;
                        bool f0 = (t0 == RirF), f1 = (t1 == RirF);
                        if (f0 != f1) {   // mélange entier × flottant : promotion explicite
                            if (!f0 && (t0 == RirZ || t0 == RirU)) x0 = "rose_vers_double(" + a0 + ")";
                            if (!f1 && (t1 == RirZ || t1 == RirU)) x1 = "rose_vers_double(" + a1 + ")";
                        }
                        out << "    " << dest << " = (" << x0 << " " << cmpop << " " << x1 << ");\n";
                    }
                }
                else if (op == "add.z") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    // Handle const
                    if (instr.args[1].kind == RirOp::Const) {
                        if (instr.args[1].name.rfind("@c", 0) == 0) a1 = instr.args[1].name.substr(2);
                    }
                    out << "    " << dest << " = " << a0 << " + " << a1 << ";\n";
                }
                else if (op == "add.f") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    { RirType p0 = ty_of(instr.args[0]), p1 = ty_of(instr.args[1]);
                      RirType dty = RirU; auto td = temp_types.find(instr.dest); if (td != temp_types.end()) dty = td->second;
                      if (p0 != RirF) a0 = "rose_vers_double(" + a0 + ")";
                      if (p1 != RirF) a1 = "rose_vers_double(" + a1 + ")"; }
                    out << "    " << dest << " = " << a0 << " + " << a1 << ";\n";
                }
                else if (op == "sub.f") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    { RirType p0 = ty_of(instr.args[0]), p1 = ty_of(instr.args[1]);
                      RirType dty = RirU; auto td = temp_types.find(instr.dest); if (td != temp_types.end()) dty = td->second;
                      if (p0 != RirF) a0 = "rose_vers_double(" + a0 + ")";
                      if (p1 != RirF) a1 = "rose_vers_double(" + a1 + ")"; }
                    out << "    " << dest << " = " << a0 << " - " << a1 << ";\n";
                }
                else if (op == "mul.f") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    { RirType p0 = ty_of(instr.args[0]), p1 = ty_of(instr.args[1]);
                      RirType dty = RirU; auto td = temp_types.find(instr.dest); if (td != temp_types.end()) dty = td->second;
                      if (p0 != RirF) a0 = "rose_vers_double(" + a0 + ")";
                      if (p1 != RirF) a1 = "rose_vers_double(" + a1 + ")"; }
                    out << "    " << dest << " = " << a0 << " * " << a1 << ";\n";
                }
                else if (op == "div.f") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    { RirType p0 = ty_of(instr.args[0]), p1 = ty_of(instr.args[1]);
                      RirType dty = RirU; auto td = temp_types.find(instr.dest); if (td != temp_types.end()) dty = td->second;
                      if (p0 != RirF) a0 = "rose_vers_double(" + a0 + ")";
                      if (p1 != RirF) a1 = "rose_vers_double(" + a1 + ")"; }
                    out << "    " << dest << " = rose_div_f(" << a0 << ", " << a1 << ");\n";
                }
                else if (op == "neg.f") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    { RirType p0 = ty_of(instr.args[0]); RirType dty = RirU; auto td = temp_types.find(instr.dest); if (td != temp_types.end()) dty = td->second;
                      if ((p0 == RirZ || p0 == RirU) && dty == RirF) a0 = "rose_vers_double(" + a0 + ")"; }
                    out << "    " << dest << " = -" << a0 << ";\n";
                }
                else if (op == "sub.z") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    if (instr.args[1].kind == RirOp::Const) {
                        if (instr.args[1].name.rfind("@c", 0) == 0) a1 = instr.args[1].name.substr(2);
                    }
                    out << "    " << dest << " = " << a0 << " - " << a1 << ";\n";
                }
                else if (op == "mul.z") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    if (instr.args[1].kind == RirOp::Const) {
                        if (instr.args[1].name.rfind("@c", 0) == 0) a1 = instr.args[1].name.substr(2);
                    }
                    out << "    " << dest << " = " << a0 << " * " << a1 << ";\n";
                }
                else if (op == "div.z" || op == "div.q" || op == "div.z.z" || op == "div.q.q") {
                    // Division exacte de Rose : flottant sinon RATIONNEL.
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    // M4.2 mixte : un operande flottant bascule en double (miroir).
                    { RirType p0 = ty_of(instr.args[0]), p1 = ty_of(instr.args[1]);
                      if (p0 == RirF || p1 == RirF) {
                          if (p0 != RirF) a0 = "rose_vers_double(" + a0 + ")";
                          if (p1 != RirF) a1 = "rose_vers_double(" + a1 + ")";
                          out << "    " << dest << " = " << a0 << " / " << a1 << ";\n";
                      } else
                    out << "    " << dest << " = rose_q_div(rose_q_de(" << a0 << "), rose_q_de(" << a1 << "));\n"; }
                }
                else if (op.rfind("divf", 0) == 0) {
                    // Division euclidienne (plancher).
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    RirType t0 = ty_of(instr.args[0]), t1 = ty_of(instr.args[1]);
                    if (t0 == RirF || t1 == RirF) {
                        std::string x0 = a0, x1 = a1;
                        if (t0 != RirF && (t0 == RirZ || t0 == RirU)) x0 = "rose_vers_double(" + a0 + ")";
                        if (t1 != RirF && (t1 == RirZ || t1 == RirU)) x1 = "rose_vers_double(" + a1 + ")";
                        out << "    " << dest << " = rose_divflo_f(" << x0 << ", " << x1 << ");\n";
                    }
                    else if (t0 == RirQ || t1 == RirQ)
                        out << "    " << dest << " = rose_div_florentin_q(" << a0 << ", " << a1 << ");\n";
                    else if (t0 == RirGd || t1 == RirGd)
                        out << "    " << dest << " = rose_div_florentin_gd(" << a0 << ", " << a1 << ");\n";
                    else
                        out << "    " << dest << " = rose_div_florentin(" << a0 << ", " << a1 << ");\n";
                }
                else if (op.rfind("add.q", 0) == 0 || op.rfind("sub.q", 0) == 0 || op.rfind("mul.q", 0) == 0) {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    char o = op[0] == 'a' ? '+' : op[0] == 's' ? '-' : '*';
                    // M4.2 mixtes Q⊛F : miroir de l'interprete — des qu'un
                    // operande est flottant (ou la destination typed F), le
                    // resultat se calcule en double sur conversions mpq_get_d.
                    { RirType p0 = ty_of(instr.args[0]), p1 = ty_of(instr.args[1]);
                      RirType dty = RirU; auto td = temp_types.find(instr.dest); if (td != temp_types.end()) dty = td->second;
                      if (p0 == RirF || p1 == RirF || dty == RirF) {
                          if (p0 != RirF) a0 = "rose_vers_double(" + a0 + ")";
                          if (p1 != RirF) a1 = "rose_vers_double(" + a1 + ")";
                      } }
                    out << "    " << dest << " = " << a0 << " " << o << " " << a1 << ";\n";
                }
                else if (op.rfind("pow", 0) == 0) {
                    // M4.2 : miroir exact de l'interprete (rose.cpp, pow) —
                    // exposant flottant D'ABORD, puis base flottante, puis
                    // domaine exact (rose_pow_exact → rationnel, comme vers_q).
                    // Suffixe RIR = type attendu du checker (pow.f/pow.q/…).
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    RirType p0 = ty_of(instr.args[0]), p1 = ty_of(instr.args[1]);
                    if (instr.args[0].kind == RirOp::Const && instr.args[0].name.rfind("@c", 0) == 0) a0 = instr.args[0].name.substr(2);
                    if (instr.args[1].kind == RirOp::Const && instr.args[1].name.rfind("@c", 0) == 0) a1 = instr.args[1].name.substr(2);
                    std::string suffixe = op.size() > 4 ? op.substr(4) : "";
                    RirType dty = RirU; auto td = temp_types.find(instr.dest); if (td != temp_types.end()) dty = td->second;
                    bool destF = (dty == RirF || suffixe == "f");
                    bool destQ = (dty == RirQ || suffixe == "q");
                    std::string base_q = p0 == RirQ ? a0 : p0 == RirGd ? "mpq_class(" + a0 + ")" : "rose_q_de(" + a0 + ")";
                    if (p1 == RirF) {
                        if (p0 != RirF) a0 = "rose_vers_double(" + a0 + ")";
                        out << "    " << dest << " = std::pow(" << a0 << ", " << a1 << ");\n";
                    } else if (p0 == RirF) {
                        if (p1 != RirF) a1 = "rose_vers_double(" + a1 + ")";
                        out << "    " << dest << " = std::pow(" << a0 << ", " << a1 << ");\n";
                    } else if (destF || destQ) {
                        out << "    " << dest << " = " << (destF ? "rose_vers_double(" : "")
                            << "rose_pow_exact(" << base_q << ", rose_vers_mpz(" << a1 << "))"
                            << (destF ? ")" : "") << ";\n";
                    } else {
                        // Destination entiere : fidele seulement si l'exposant
                        // litteral est >= 0 (sinon l'interprete produit un
                        // RATIONNEL que le slot ne peut porter) → rejet poli.
                        // Le checker materialise les constantes en temps
                        // cst.z — on remonte donc au producteur du temp.
                        auto cst_z_nonneg = [&](const RirOp &o) -> bool {
                            auto digits = [](const std::string &t) {
                                // entier littéral « 123 » ou fraction positive
                                // « 123/456 » (floor ≥ 0 sans signe moins).
                                if (t.empty() || t.find_first_of("-.eE") != std::string::npos) return false;
                                auto slash = t.find('/');
                                const std::string num = slash == std::string::npos ? t : t.substr(0, slash);
                                const std::string den = slash == std::string::npos ? "1" : t.substr(slash + 1);
                                if (num.empty() || den.empty()) return false;
                                for (char c : num) if (c < '0' || c > '9') return false;
                                for (char c : den) if (c < '0' || c > '9') return false;
                                return true;
                            };
                            if (o.kind == RirOp::Const)
                                return o.name.rfind("@c", 0) == 0 && digits(o.name.substr(2));
                            if (o.kind == RirOp::Temp)
                                for (auto &b2 : f.blocs) for (auto &i2 : b2.instrs)
                                    if (i2.dest == o.name)
                                        return i2.op.rfind("cst.", 0) == 0 && !i2.args.empty()
                                            && i2.args[0].kind == RirOp::Const
                                            && i2.args[0].name.rfind("@c", 0) == 0
                                            && digits(i2.args[0].name.substr(2));
                            return false;
                        };
                        if (!cst_z_nonneg(instr.args[1]))
                            throw std::runtime_error("AOT : puissance a exposant non litteral positif dans un emplacement entier — l'interprete peut produire un rationnel");
                        out << "    " << dest << " = rose_puissance_gd(" << a0 << ", " << a1 << ");\n";
                    }
                }
                else if (op.rfind("rem.z", 0) == 0) {
                    // Modulo à la Rose : plancher (miroir mpz_fdiv_r).
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    if (ty_of(instr.args[0]) == RirGd || ty_of(instr.args[1]) == RirGd)
                        out << "    " << dest << " = rose_rem_florentin_gd(" << a0 << ", " << a1 << ");\n";
                    else
                        out << "    " << dest << " = rose_rem_florentin(" << a0 << ", " << a1 << ");\n";
                }
                else if (op == "neg.z") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    out << "    " << dest << " = -" << a0 << ";\n";
                }
                else if (op == "ret.u" || op == "ret" || op == "ret.z" || op == "ret.f" || op == "ret.b" || op == "ret.s") {
                    if (!instr.args.empty()) {
                        std::string v = instr.args[0].kind == RirOp::Temp ? L(instr.args[0])
                                                            : cpp_operand(instr.args[0], temp_types, &flagpees);
                        RirType aty = RirU;
                        if (instr.args[0].kind == RirOp::Temp && temp_types.count(instr.args[0].name))
                            aty = temp_types[instr.args[0].name];
                        if (f.retour == RirZ && aty == RirF) v = "rose_depuis_double(" + v + ")";
                        else if (f.retour == RirF && aty == RirZ) v = "rose_vers_double(" + v + ")";
                        else if (f.retour == RirGd && (aty == RirZ || aty == RirU)) v = "rose_vers_mpz(" + v + ")";
                        out << "    return " << v << ";\n";
                    } else {
                        out << "    return 0;\n";
                    }
                }
                else if (op == "add.s") {
                    std::string dest = sanitize(instr.dest);
                    std::string a0 = L(instr.args[0]);
                    std::string a1 = L(instr.args[1]);
                    if (instr.args[1].kind == RirOp::Const) {
                        std::string s = instr.args[1].name;
                        std::string escaped;
                        for (char c : s) {
                            if (c == '"') escaped += "\\\"";
                            else if (c == '\\') escaped += "\\\\";
                            else escaped += c;
                        }
                        a1 = "\"" + escaped + "\"";
                    }
                    out << "    " << dest << " = " << a0 << " + " << a1 << ";\n";
                }
                else {
                    // M4.2 contrat : opération RIR inconnue = rejet explicite,
                    // jamais une absence d'émission (silencieux = pire que rejet).
                    throw std::runtime_error("AOT : opération RIR « " + op + " » non transpilée");
                }
            }
        }

        if (fname == "main") {
            out << "    return 0;\n";
        }
        out << "}\n\n";
    }

    return out.str();
}
