/* ═══════════════════════ Rose v2 — moteur natif ═══════════════════════
 * Compilateur + interpréteur autonome du langage Rose.
 * Binaire unique : le moteur (C++/GMP) embarque lexeur, parseur,
 * vérificateur, repli de constantes, interpréteur, bibliothèque
 * mathématique exacte (GMP liée statiquement) et FFI.
 * `rose comp prog.rose` produit un exécutable autonome (moteur + programme).
 * Aucune dépendance à l'exécution hormis la libc.
 * ======================================================================= */
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "transpiler.h"
#include <cstring>
#include <climits>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif
#include <sys/stat.h>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <utility>
#include <vector>
#include <gmp.h>
#include <gmpxx.h>

#ifdef _WIN32
/* Couche FFI portative : la API dynamique Win32 habillée en dlopen. */
#define RTLD_NOW 0
#define RTLD_LOCAL 0
static void *dlopen(const char *nom, int) { return (void *)LoadLibraryA(nom); }
static void *dlsym(void *h, const char *s) { return (void *)GetProcAddress((HMODULE)h, s); }
static void dlclose(void *h) { FreeLibrary((HMODULE)h); }
#include <filesystem>
/* fopen en char* est ANSI (ACP) sous Windows : un chemin UTF-8 contenant
   un accent échouerait. filesystem::path traite l'étroit en UTF-8 et
   l'ouverture passe par _wfopen : les chemins Unicode fonctionnent. */
#define ROSE_CHEMIN(s) std::filesystem::path(s)
#else
#define ROSE_CHEMIN(s) (s)
#endif

/* Conversions entier↔long long sans passer par « long » : GMP renvoie un
   long (32 bits sous Windows, 64 bits sur Linux/ARM). import/export d'un mot
   de 8 octets natif garde la valeur exacte modulo 2^64 sur toutes les
   plateformes, avec la même sémantique de troncature que l'ancien code. */
static long long z_vers_ll(mpz_srcptr z) {
    unsigned long long u = 0;
    size_t cnt = 1;
    mpz_export(&u, &cnt, -1, sizeof(unsigned long long), 0, 0, z);
    return mpz_sgn(z) < 0 ? -(long long)u : (long long)u;
}
static mpz_class z_depuis_ll(long long v) {
    mpz_class r;
    unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
    mpz_import(r.get_mpz_t(), 1, -1, sizeof(u), 0, 0, &u);
    if (v < 0) mpz_neg(r.get_mpz_t(), r.get_mpz_t());
    return r;
}

/* ── Chaîne v3 : indexation par CARACTÈRES Unicode ─────────────────── */
/* Position 0 = premier caractère. ordre/caractere manipulent des points
   de code ; sous_chaine découpe en caractères. Le contenu est supposé
   UTF-8 valide (entrées invalides = comportement non défini). */
static std::vector<std::string> ARGUMENTS_PROG;   /* les argv du programme */

struct Quitter { int code; };

static size_t utf8_seq(unsigned char c) {
    return (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 :
           (c & 0xE0) == 0xC0 ? 2 : 1;
}
static size_t utf8_len(const std::string &s) {
    size_t k = 0;
    for (size_t i = 0; i < s.size(); i++) if ((s[i] & 0xC0) != 0x80) k++;
    return k;
}
/* octet où commence le k-ième caractère (utf8_off(len) = s.size()) */
static size_t utf8_off(const std::string &s, size_t k) {
    size_t vu = 0;
    for (size_t i = 0; i < s.size(); i++)
        if ((s[i] & 0xC0) != 0x80) { if (vu == k) return i; vu++; }
    return s.size();
}
static unsigned long utf8_char_at(const std::string &s, size_t k) {
    size_t o = utf8_off(s, k);
    unsigned char c = (unsigned char)s[o];
    size_t q = utf8_seq(c);
    unsigned long v = (q == 1) ? c : (c & (0xFFu >> (q + 1)));
    for (size_t i = 1; i < q && o + i < s.size(); i++)
        v = (v << 6) | ((unsigned char)s[o + i] & 0x3F);
    return v;
}
static std::string utf8_char_str(const std::string &s, size_t k) {
    size_t o = utf8_off(s, k);
    return s.substr(o, utf8_seq((unsigned char)s[o]));
}
static std::string utf8_encode(unsigned long cp) {
    std::string r;
    if (cp < 0x80) r += (char)cp;
    else if (cp < 0x800) {
        r += (char)(0xC0 | (cp >> 6));      r += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        r += (char)(0xE0 | (cp >> 12));     r += (char)(0x80 | ((cp >> 6) & 0x3F));
        r += (char)(0x80 | (cp & 0x3F));
    } else {
        r += (char)(0xF0 | (cp >> 18));     r += (char)(0x80 | ((cp >> 12) & 0x3F));
        r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F));
    }
    return r;
}



/* ── erreur localisée ─────────────────────────────────────────────── */
struct ErreurRose {
    std::string message;
    int ligne = 0, colonne = 0;
    ErreurRose(std::string m, int l = 0, int c = 0) : message(std::move(m)), ligne(l), colonne(c) {}
    std::string texte() const {
        if (ligne) {
            std::string p = "[Ligne " + std::to_string(ligne) + ", Colonne " + std::to_string(colonne) + "] ";
            return p + message;
        }
        return message;
    }
};

/* ── UTF-8 → codepoints (comme les chaînes Python) ────────────────── */
static std::vector<uint32_t> utf8_vers_cp(const std::string &s) {
    std::vector<uint32_t> cps;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int l;
        if (c < 0x80) { cp = c; l = 1; }
        else if ((c >> 5) == 0x6 && i + 1 < n) { cp = c & 0x1F; l = 2; }
        else if ((c >> 4) == 0xE && i + 2 < n) { cp = c & 0x0F; l = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < n) { cp = c & 0x07; l = 4; }
        else { cps.push_back(0x0E0000u + c); i++; continue; }   /* octet invalide isolé */
        for (int k = 1; k < l; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        cps.push_back(cp);
        i += (size_t)l;
    }
    return cps;
}

static void cp_vers_utf8(uint32_t cp, std::string &out) {
    if (cp < 0x80) { out += (char)cp; return; }
    if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); return; }
    if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
        return;
    }
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
}

static std::string cp_tranche(const std::vector<uint32_t> &cps, size_t a, size_t b) {
    std::string s;
    for (size_t i = a; i < b && i < cps.size(); i++) cp_vers_utf8(cps[i], s);
    return s;
}

static bool cp_lettre(uint32_t c) {
    if (c < 0x80) return std::isalpha((int)c) != 0 || c == '_';
    return (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF)
        || (c >= 0x370 && c <= 0x4FF) || (c >= 0x1E00 && c <= 0x1FFF);
}

static bool cp_ident_suite(uint32_t c) {
    if (c < 0x80) return std::isalnum((int)c) != 0 || c == '_';
    return cp_lettre(c);
}

static bool cp_chiffre(uint32_t c) { return c >= '0' && c <= '9'; }

/* ── jetons ───────────────────────────────────────────────────────── */
enum class Tk {
    Eof, Ident, Nombre, Flottant, Chaine,
    Plus, Moins, Etoile, Slash, DeuxPts, Percent, Chapeau, Bang, Point, Virgule,
    DivEntier, Affect, LParen, RParen, LBrace, RBrace, LBracket, RBracket, Semi,
    DeuxPoints, FlecheD, FlecheG, Tilde, Arobase, PointDInterro, Question, Racine,
    Inf, Sup, InfEg, SupEg, DoubleEg, Diff, Environ, QMarkEg, TildeEg, Mot
};

struct Token { Tk type; std::string texte; int ligne; int colonne; };

/* paires (2 codepoints) — table SYMB_OPR multi-caractères */
static Tk paire_jetons(uint32_t a, uint32_t b) {
    if (a == '<' && b == '=') return Tk::InfEg;
    if (a == '>' && b == '=') return Tk::SupEg;
    if (a == '=' && b == '=') return Tk::DoubleEg;
    if (a == '!' && b == '=') return Tk::Diff;
    if (a == '/' && b == '=') return Tk::Diff;
    if (a == '?' && b == '=') return Tk::QMarkEg;
    if (a == '=' && b == '~') return Tk::TildeEg;
    if (a == '~' && b == '=') return Tk::TildeEg;
    return (Tk)(-1);
}
/* opérateurs monocaractères non-ASCII */
static Tk uni_jeton(uint32_t c) {
    switch (c) {
        case 0x2248: return Tk::Environ;   /* ≈ */
        case 0x2264: return Tk::InfEg;     /* ≤ */
        case 0x2265: return Tk::SupEg;     /* ≥ */
        case 0x2260: return Tk::Diff;      /* ≠ */
        case 0x00D7: return Tk::Etoile;    /* × */
        case 0x22C5: return Tk::Etoile;    /* ⋅ */
        case 0x00B7: return Tk::Etoile;    /* · */
        case 0x00F7: return Tk::DivEntier; /* ÷ */
        case 0x2190: return Tk::Affect;         /* ← affectation */
        case 0x2192: return Tk::PointDInterro;  /* → signature */
        case 0x221A: return Tk::Racine;         /* √ */
        default: return (Tk)(-1);
    }
}
static Tk car_jeton(uint32_t c) {
    switch (c) {
        case '+': return Tk::Plus; case '-': return Tk::Moins; case '*': return Tk::Etoile;
        case '/': return Tk::Slash; case ':': return Tk::DeuxPts; case '%': return Tk::Percent;
        case '^': return Tk::Chapeau; case '!': return Tk::Bang; case '.': return Tk::Point;
        case ',': return Tk::Virgule; case '?': return Tk::Question;
        case '(': return Tk::LParen; case ')': return Tk::RParen;
        case '{': return Tk::LBrace; case '}': return Tk::RBrace;
        case '[': return Tk::LBracket; case ']': return Tk::RBracket;
        case ';': return Tk::Semi; case '<': return Tk::Inf; case '>': return Tk::Sup;
        case '~': return Tk::Tilde; case '@': return Tk::Arobase;
        default: return (Tk)(-1);
    }
}
static bool est_mot_cle(const std::string &s) {
    static const char *MOTS[] = {"fn","retour","si","sinon","tant_que","pour","de","vers",
        "vrai","faux","et","ou","non","div","entier","rationnel","flottant","booleen",
        "chaine","liste","table", nullptr};
    for (int i = 0; MOTS[i]; i++) if (s == MOTS[i]) return true;
    return false;
}

static std::vector<Token> tokeniser(const std::string &source) {
    std::vector<uint32_t> cps = utf8_vers_cp(source);
    std::vector<Token> tokens;
    size_t i = 0, n = cps.size();
    int ligne = 1, col = 1;
    auto avancer = [&](size_t k) {
        for (size_t u = 0; u < k; u++) {
            if (i < n && cps[i] == '\n') { ligne++; col = 1; } else col++;
            i++;
        }
    };
    while (i < n) {
        uint32_t c = cps[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { avancer(1); continue; }
        if (c == '#' || (c == '/' && i + 1 < n && cps[i + 1] == '/')) {
            while (i < n && cps[i] != '\n') avancer(1);
            continue;
        }
        if (c == '"' || c == '\'') {
            uint32_t quote = c;
            int dl = ligne, dc = col;
            avancer(1);
            std::string buf;
            for (;;) {
                if (i >= n) throw ErreurRose("Chaîne non terminée", dl, dc);
                if (cps[i] == '\\') {
                    if (i + 1 >= n) throw ErreurRose("Échappement en fin de fichier", ligne, col);
                    uint32_t nx = cps[i + 1];
                    if (nx == 'n') buf += '\n';
                    else if (nx == 't') buf += '\t';
                    else if (nx == '\\' || nx == '"' || nx == '\'') buf += (char)nx;
                    else { buf += '\\'; cp_vers_utf8(nx, buf); }
                    avancer(2);
                    continue;
                }
                if (cps[i] == quote) { avancer(1); break; }
                cp_vers_utf8(cps[i], buf);
                avancer(1);
            }
            tokens.push_back({Tk::Chaine, buf, dl, dc});
            continue;
        }
        if (cp_chiffre(c)) {
            int dl = ligne, dc = col;
            size_t debut = i;
            while (i < n && cp_chiffre(cps[i])) avancer(1);
            bool flott = false;
            if (i + 1 < n && cps[i] == '.' && cp_chiffre(cps[i + 1])) {
                flott = true;
                avancer(1);
                while (i < n && cp_chiffre(cps[i])) avancer(1);
            }
            tokens.push_back({flott ? Tk::Flottant : Tk::Nombre, cp_tranche(cps, debut, i), dl, dc});
            continue;
        }
        if (c == 0x03C0) {   /* π : constante 3,14159… (flottant) */
            tokens.push_back({Tk::Flottant, "3.141592653589793", ligne, col});
            avancer(1); continue;
        }
        if (c == '$' || cp_lettre(c)) {
            int dl = ligne, dc = col;
            size_t debut = i;
            bool dollar = false;
            if (c == '$') { dollar = true; avancer(1); }
            if (i >= n || !cp_lettre(cps[i]))
                throw ErreurRose("Attente d'un nom de variable après '$'", dl, dc);
            while (i < n && cp_ident_suite(cps[i])) avancer(1);
            std::string texte = cp_tranche(cps, debut, i);
            std::string nu = dollar ? texte.substr(1) : texte;
            if (dollar || est_mot_cle(nu))
                tokens.push_back({Tk::Ident, nu, dl, dc});
            else
                tokens.push_back({Tk::Mot, texte, dl, dc});
            continue;
        }
        if (i + 1 < n) {
            Tk tt = paire_jetons(cps[i], cps[i + 1]);
            if ((int)tt != -1) {
                tokens.push_back({tt, cp_tranche(cps, i, i + 2), ligne, col});
                avancer(2);
                continue;
            }
        }
        if (c == '>' && i + 1 < n && cps[i + 1] == '>') {
            tokens.push_back({Tk::FlecheD, ">>", ligne, col}); avancer(2); continue;
        }
        if (c == '<' && i + 1 < n && cps[i + 1] == '<') {
            tokens.push_back({Tk::FlecheG, "<<", ligne, col}); avancer(2); continue;
        }
        if (c == '-' && i + 1 < n && cps[i + 1] == '>') {
            tokens.push_back({Tk::PointDInterro, "->", ligne, col}); avancer(2); continue;
        }
        if (c == '=') { tokens.push_back({Tk::Affect, "=", ligne, col}); avancer(1); continue; }
        if (c == ':' && i + 1 < n && cps[i + 1] == ':') {
            tokens.push_back({Tk::DeuxPoints, "::", ligne, col}); avancer(2); continue;
        }
        if (c == 0x00B2 || c == 0x00B3) {   /* ² ³ : x²  ≡  x^2 */
            int d = (c == 0x00B2) ? 2 : 3;
            tokens.push_back({Tk::Chapeau, "^", ligne, col});
            tokens.push_back({Tk::Nombre, d == 2 ? "2" : "3", ligne, col});
            avancer(1); continue;
        }
        {
            Tk tt = uni_jeton(c);
            if ((int)tt != -1) { tokens.push_back({tt, cp_tranche(cps, i, i + 1), ligne, col}); avancer(1); continue; }
        }
        {
            Tk tt = car_jeton(c);
            if ((int)tt != -1) { tokens.push_back({tt, cp_tranche(cps, i, i + 1), ligne, col}); avancer(1); continue; }
        }
        throw ErreurRose("Caractère inattendu '" + cp_tranche(cps, i, i + 1) + "'", ligne, col);
    }
    tokens.push_back({Tk::Eof, "", ligne, col});
    return tokens;
}

/* ══════════ M4.0 — oracle de lexage : « rose tokens » ══════════
   Le lexeur auto-hébergé (rose/lexeur.rose) doit reproduire CETTE
   sortie à l'octet près : c'est le point d'appui du bootstrap M4.
   Format : « ligne:colonne type texte_échappé », un token par ligne. */
static const char *tk_nom(Tk t) {
    switch (t) {
        case Tk::Eof: return "eof"; case Tk::Ident: return "ident";
        case Tk::Nombre: return "nombre"; case Tk::Flottant: return "flottant";
        case Tk::Chaine: return "chaine"; case Tk::Plus: return "+";
        case Tk::Moins: return "-"; case Tk::Etoile: return "*";
        case Tk::Slash: return "/"; case Tk::DeuxPts: return ":";
        case Tk::Percent: return "%"; case Tk::Chapeau: return "^";
        case Tk::Bang: return "!"; case Tk::Point: return ".";
        case Tk::Virgule: return ","; case Tk::DivEntier: return "div";
        case Tk::Affect: return "="; case Tk::LParen: return "(";
        case Tk::RParen: return ")"; case Tk::LBrace: return "{";
        case Tk::RBrace: return "}"; case Tk::LBracket: return "[";
        case Tk::RBracket: return "]"; case Tk::Semi: return ";";
        case Tk::DeuxPoints: return "::"; case Tk::FlecheD: return ">>";
        case Tk::FlecheG: return "<<"; case Tk::Tilde: return "~";
        case Tk::Arobase: return "@"; case Tk::PointDInterro: return "->";
        case Tk::Question: return "?"; case Tk::Racine: return "racine";
        case Tk::Inf: return "<"; case Tk::Sup: return ">";
        case Tk::InfEg: return "<="; case Tk::SupEg: return ">=";
        case Tk::DoubleEg: return "=="; case Tk::Diff: return "!=";
        case Tk::Environ: return "≈"; case Tk::QMarkEg: return "?=";
        case Tk::TildeEg: return "=~"; case Tk::Mot: return "mot";
    }
    return "?";
}
static std::string texte_echappe(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if ( c == '\t') o += "\\t";
        else if (c == '\x1f') o += "\\u241f";  // séparateur interne du jalon M4.1
        else o += c;
    }
    return o;
}

/* ── types Rose ───────────────────────────────────────────────────── */
enum : int {
    T_VIDE = 0, T_ENTIER = 1, T_RATIONNEL = 2, T_FLOTTANT = 3,
    T_BOOLEEN = 4, T_CHAINE = 5, T_LISTE = 6, T_TABLE = 7, T_VALEUR = 8,
    T_TAMPON = 9
};

struct Ty {
    int n = T_VIDE;
    int elt = T_VIDE;                       /* élément si liste */
    Ty() = default;
    Ty(int nn) : n(nn) {}
    static Ty liste(int e) { Ty t(T_LISTE); t.elt = e; return t; }
    bool operator==(const Ty &o) const { return n == o.n && (n != T_LISTE || elt == o.elt); }
    bool operator!=(const Ty &o) const { return !(*this == o); }
    std::string nom() const {
        switch (n) {
            case T_ENTIER: return "entier"; case T_RATIONNEL: return "rationnel";
            case T_FLOTTANT: return "flottant"; case T_BOOLEEN: return "booleen";
            case T_CHAINE: return "chaine"; case T_VIDE: return "vide";
            case T_LISTE: return "liste<" + Ty(elt).nom() + ">";
            case T_TABLE: return "table";
            case T_TAMPON: return "tampon";
            case T_VALEUR: return "valeur";
        }
        return "?";
    }
};

/* ── AST expressions ──────────────────────────────────────────────── */
enum class NK { Litteral, ChaineLit, BooleenLit, ListeLit, Var, Appel, Index,
                Binaire, Unaire, Factorielle };

struct E;
using PE = std::shared_ptr<E>;

struct E {
    NK k;
    int ligne = 0;
    std::string texte;              /* Litteral / ChaineLit / Var / nom d'appel */
    std::string module;             /* préfixe module:: (vide = aucun) */
    bool flag = false;              /* flottant / booléen */
    std::vector<PE> args;           /* appel / liste */
    PE a, b;                        /* gauche/opérande, droite/index */
    int type = T_VIDE;              /* annotation du vérificateur */
    int elt_type = T_VIDE;          /* élément de liste annoté */
    E(NK kk, int l) : k(kk), ligne(l) {}
};
static PE neo(NK k, int l) { return std::make_shared<E>(k, l); }

/* ── AST instructions ─────────────────────────────────────────────── */
enum class IK { Afficher, Lire, Affectation, Si, TantQue, Pour, Retour, Expr, Bloc };

struct Bloc;
using PB = std::shared_ptr<Bloc>;
struct I;
using PI = std::shared_ptr<I>;

struct Bloc {
    int ligne = 0;
    std::vector<PI> instrs;
};

struct I {
    IK k;
    int ligne = 0;
    std::string nom;
    PE expr, test, debut, fin;              /* selon le cas */
    PB alors, sinon, corps;
    std::vector<PI> instrs;                 /* Bloc en instruction */
    int type_read = T_ENTIER;
    I(IK kk, int l) : k(kk), ligne(l) {}
};
static PI ino(IK k, int l) { return std::make_shared<I>(k, l); }

struct Signature {
    std::string nom;
    std::vector<std::pair<std::string, Ty>> params;
    Ty retour;                              /* n==T_VIDE + a_retour pour absence */
    bool a_retour = false;
    PB corps;                               /* nullptr = externe */
    std::string dll, dll_son, symbole;
    int ligne = 0;
};

struct Programme {
    std::map<std::string, std::shared_ptr<Signature>> signatures;
    std::vector<PI> instructions;
};

/* ── parser à descente récursive ──────────────────────────────────── */
struct Ann { std::string dll, dll_son, sym; bool active = false; };

struct Parser {
    std::vector<Token> tokens;
    size_t pos = 0;

    explicit Parser(const std::string &source) : tokens(tokeniser(source)) {}

    const Token &voir(size_t d = 0) const {
        size_t p = pos + d;
        return tokens[p < tokens.size() ? p : tokens.size() - 1];
    }
    Token suivant() {
        Token t = voir();
        if (t.type != Tk::Eof) pos++;
        return t;
    }
    bool est(std::initializer_list<Tk> tt) const {
        for (Tk x : tt) if (voir().type == x) return true;
        return false;
    }
    bool est_mot(const std::string &texte) const {
        const Token &t = voir();
        return (t.type == Tk::Ident || t.type == Tk::Mot) && t.texte == texte;
    }
    bool accepter(std::initializer_list<Tk> tt) {
        if (est(tt)) { suivant(); return true; }
        return false;
    }
    bool accepter_mot(const std::string &texte) {
        if (est_mot(texte)) { suivant(); return true; }
        return false;
    }
    Token exiger(Tk tt, const std::string &quoi) {
        Token t = suivant();
        if (t.type != tt)
            throw ErreurRose("Attendu " + quoi + ", trouvé '" + t.texte + "'", t.ligne, t.colonne);
        return t;
    }
    std::string nom_ident() {
        Token t = suivant();
        if (t.type != Tk::Ident && t.type != Tk::Mot)
            throw ErreurRose("Attendu un identifiant, trouvé '" + t.texte + "'", t.ligne, t.colonne);
        return t.texte;
    }

    /* programme */
    Programme programmer() {
        Programme prog;
        Ann ann;
        while (!est({Tk::Eof})) {
            if (accepter({Tk::Semi})) continue;
            if (voir().type == Tk::Arobase
                    && (voir(1).type == Tk::Ident || voir(1).type == Tk::Mot)
                    && (voir(1).texte == "dll" || voir(1).texte == "sym")) {
                suivant();
                std::string genre = nom_ident();
                exiger(Tk::LParen, "'(' après @dll/@sym");
                if (genre == "dll") {
                    ann.dll = exiger(Tk::Chaine, "nom de bibliothèque").texte;
                    if (accepter({Tk::Virgule}))
                        ann.dll_son = exiger(Tk::Chaine, "nom de fichier").texte;
                } else {
                    ann.sym = exiger(Tk::Chaine, "nom de symbole").texte;
                }
                exiger(Tk::RParen, "')'");
                ann.active = true;
                continue;
            }
            if (est_mot("fn")) {
                auto sig = def_fonction(ann);
                ann = {};
                if (prog.signatures.count(sig->nom))
                    throw ErreurRose("Fonction '" + sig->nom + "' définie deux fois", sig->ligne);
                prog.signatures[sig->nom] = sig;
                continue;
            }
            if (ann.active)
                throw ErreurRose("Annotation @dll/@sym sans fonction juste après",
                                 voir().ligne, voir().colonne);
            prog.instructions.push_back(instruction());
        }
        return prog;
    }

    std::shared_ptr<Signature> def_fonction(Ann &ann) {
        Token tok = suivant();                       /* 'fn' */
        auto sig = std::make_shared<Signature>();
        sig->nom = nom_ident();
        exiger(Tk::LParen, "'('");
        if (!est({Tk::RParen})) {
            for (;;) {
                std::string pname = nom_ident();
                exiger(Tk::DeuxPts, "':' (nom: type)");
                Ty pt = type_rose();
                sig->params.emplace_back(pname, pt);
                if (!accepter({Tk::Virgule})) break;
            }
        }
        exiger(Tk::RParen, "')'");
        if (accepter({Tk::PointDInterro})) { sig->retour = type_rose(); sig->a_retour = true; }
        if (est({Tk::LBrace})) {
            sig->corps = bloc();
            if (!sig->a_retour) { sig->retour = Ty(T_VIDE); sig->a_retour = true; }
        } else {
            if (!sig->a_retour)
                throw ErreurRose("Une fonction externe doit déclarer un type de retour", tok.ligne);
        }
        sig->dll = ann.dll; sig->dll_son = ann.dll_son; sig->symbole = ann.sym;
        sig->ligne = tok.ligne;
        return sig;
    }

    Ty type_rose() {
        Token t = suivant();
        if (t.type != Tk::Ident && t.type != Tk::Mot)
            throw ErreurRose("Attendu un type, trouvé '" + t.texte + "'", t.ligne, t.colonne);
        if (t.texte == "liste") {
            exiger(Tk::Inf, "'<' de liste<T>");
            Ty elt = type_rose();
            exiger(Tk::Sup, "'>' de liste<T>");
            return Ty::liste(elt.n);
        }
        if (t.texte == "entier") return Ty(T_ENTIER);
        if (t.texte == "rationnel") return Ty(T_RATIONNEL);
        if (t.texte == "flottant") return Ty(T_FLOTTANT);
        if (t.texte == "booleen") return Ty(T_BOOLEEN);
        if (t.texte == "chaine") return Ty(T_CHAINE);
        if (t.texte == "table") return Ty(T_TABLE);
        throw ErreurRose("Type inconnu '" + t.texte + "'", t.ligne, t.colonne);
    }

    /* instructions */
    PI instruction() {
        const Token &t = voir();
        if (t.type == Tk::LBrace) {
            PB b = bloc();
            PI i = ino(IK::Bloc, t.ligne);
            i->instrs = b->instrs;
            return i;
        }
        if (t.type == Tk::FlecheD) {
            suivant();
            PI i = ino(IK::Afficher, t.ligne);
            i->expr = expression();
            return i;
        }
        if (t.type == Tk::FlecheG) {
            suivant();
            PI i = ino(IK::Lire, t.ligne);
            i->nom = nom_ident();
            return i;
        }
        if (t.type == Tk::Question) {
            suivant();
            PI i = ino(IK::Si, t.ligne);
            i->test = parentheser();
            i->alors = corps_chevron();
            return i;
        }
        if (t.type == Tk::Arobase) {
            suivant();
            PI i = ino(IK::TantQue, t.ligne);
            i->test = parentheser();
            i->corps = corps_chevron();
            return i;
        }
        if (t.type == Tk::Ident && t.texte == "si") {
            suivant();
            PI i = ino(IK::Si, t.ligne);
            i->test = parentheser();
            i->alors = bloc();
            if (accepter_mot("sinon")) {
                if (est({Tk::LBrace})) i->sinon = bloc();
                else {
                    i->sinon = std::make_shared<Bloc>();
                    i->sinon->ligne = voir().ligne;
                    i->sinon->instrs.push_back(instruction());
                }
            }
            return i;
        }
        if (t.type == Tk::Ident && t.texte == "tant_que") {
            suivant();
            PI i = ino(IK::TantQue, t.ligne);
            i->test = parentheser();
            i->corps = bloc();
            return i;
        }
        if (t.type == Tk::Ident && t.texte == "pour") {
            suivant();
            PI i = ino(IK::Pour, t.ligne);
            i->nom = nom_ident();
            if (!accepter_mot("de"))
                throw ErreurRose("Attendu 'de' dans pour…de…vers", voir().ligne, voir().colonne);
            i->debut = expression();
            if (!accepter_mot("vers"))
                throw ErreurRose("Attendu 'vers' dans pour…de…vers", voir().ligne, voir().colonne);
            i->fin = expression();
            i->corps = bloc();
            return i;
        }
        if (t.type == Tk::Ident && t.texte == "retour") {
            suivant();
            PI i = ino(IK::Retour, t.ligne);
            if (!est({Tk::Semi, Tk::RBrace, Tk::Eof})) i->expr = expression();
            return i;
        }
        if ((t.type == Tk::Ident || t.type == Tk::Mot) && voir(1).type == Tk::Affect) {
            PI i = ino(IK::Affectation, t.ligne);
            i->nom = nom_ident();
            suivant();                                  /* '=' */
            i->expr = expression();
            return i;
        }
        PI i = ino(IK::Expr, t.ligne);
        i->expr = expression();
        return i;
    }

    PE parentheser() {
        exiger(Tk::LParen, "'(' ");
        PE e = expression();
        exiger(Tk::RParen, "')'");
        return e;
    }

    PB corps_chevron() {
        Token t = exiger(Tk::LBracket, "'[' de corps");
        PB b = std::make_shared<Bloc>();
        b->ligne = t.ligne;
        while (!est({Tk::RBracket, Tk::Eof})) {
            b->instrs.push_back(instruction());
            if (!accepter({Tk::Semi})) break;
        }
        exiger(Tk::RBracket, "']' de corps");
        return b;
    }

    PB bloc() {
        Token t = exiger(Tk::LBrace, "'{'");
        PB b = std::make_shared<Bloc>();
        b->ligne = t.ligne;
        while (!est({Tk::RBrace, Tk::Eof})) {
            if (accepter({Tk::Semi})) continue;
            b->instrs.push_back(instruction());
            accepter({Tk::Semi});
        }
        exiger(Tk::RBrace, "'}'");
        return b;
    }

    /* expressions — précédences */
    PE expression() { return ou_expr(); }

    PE ou_expr() {
        PE g = et_expr();
        while (est_mot("ou")) {
            suivant();
            PE n = neo(NK::Binaire, voir().ligne); n->texte = "ou"; n->a = g; n->b = et_expr();
            g = n;
        }
        return g;
    }
    PE et_expr() {
        PE g = non_expr();
        while (est_mot("et")) {
            suivant();
            PE n = neo(NK::Binaire, voir().ligne); n->texte = "et"; n->a = g; n->b = non_expr();
            g = n;
        }
        return g;
    }
    PE non_expr() {
        if (est_mot("non")) {
            Token t = suivant();
            PE n = neo(NK::Unaire, t.ligne); n->texte = "non"; n->a = non_expr();
            return n;
        }
        return comparaison();
    }
    static const char *comp_nom(Tk tt) {
        switch (tt) {
            case Tk::Inf: return "<"; case Tk::InfEg: return "<="; case Tk::Sup: return ">";
            case Tk::SupEg: return ">="; case Tk::DoubleEg: return "=="; case Tk::Diff: return "!=";
            case Tk::Environ: return "≈"; case Tk::QMarkEg: return "?="; case Tk::TildeEg: return "=~";
            default: return nullptr;
        }
    }
    PE comparaison() {
        PE g = somme();
        for (;;) {
            const char *op = comp_nom(voir().type);
            if (!op) break;
            Token tok = suivant();
            PE n = neo(NK::Binaire, tok.ligne); n->texte = op; n->a = g; n->b = somme();
            g = n;
        }
        return g;
    }
    PE somme() {
        PE g = produit();
        while (est({Tk::Plus, Tk::Moins})) {
            Token tok = suivant();
            PE n = neo(NK::Binaire, tok.ligne);
            n->texte = tok.type == Tk::Plus ? "+" : "-";
            n->a = g; n->b = produit();
            g = n;
        }
        return g;
    }
    PE produit() {
        PE g = puissance();
        while (est({Tk::Etoile, Tk::Slash, Tk::Percent, Tk::DivEntier}) || est_mot("div")) {
            Token tok = suivant();
            std::string op;
            switch (tok.type) {
                case Tk::Etoile: op = "*"; break;
                case Tk::Slash: op = "/"; break;
                case Tk::Percent: op = "%"; break;
                default: op = "div"; break;
            }
            PE n = neo(NK::Binaire, tok.ligne); n->texte = op; n->a = g; n->b = puissance();
            g = n;
        }
        return g;
    }
    PE puissance() {
        PE g = factorielle_post();
        if (est({Tk::Chapeau})) {
            Token tok = suivant();
            PE n = neo(NK::Binaire, tok.ligne); n->texte = "^"; n->a = g; n->b = puissance();
            return n;
        }
        return g;
    }
    PE factorielle_post() {
        PE g = unaire();
        while (est({Tk::Bang})) {
            Token tok = suivant();
            PE n = neo(NK::Factorielle, tok.ligne); n->a = g;
            g = n;
        }
        return g;
    }
    PE unaire() {
        if (est({Tk::Racine})) {
            Token t = suivant();
            PE demi = neo(NK::Litteral, t.ligne); demi->texte = "0.5"; demi->flag = true;
            PE n = neo(NK::Binaire, t.ligne); n->texte = "^"; n->a = unaire(); n->b = demi;
            return n;
        }
        if (est({Tk::Moins})) {
            Token t = suivant();
            PE n = neo(NK::Unaire, t.ligne); n->texte = "-"; n->a = unaire();
            return n;
        }
        if (est({Tk::Plus})) { suivant(); return unaire(); }
        return postfixe();
    }
    PE postfixe() {
        PE g = atome();
        for (;;) {
            if (est({Tk::LParen})) {
                Token tok = suivant();
                std::string module, nom;
                if (g->k == NK::Appel && g->args.empty() && !g->module.empty()) {
                    module = g->module; nom = g->texte;
                } else if (g->k == NK::Var) {
                    nom = g->texte;
                } else {
                    throw ErreurRose("Seul un nom de fonction peut être appelé", tok.ligne, tok.colonne);
                }
                PE n = neo(NK::Appel, tok.ligne);
                n->module = module; n->texte = nom;
                if (!est({Tk::RParen})) {
                    for (;;) {
                        n->args.push_back(expression());
                        if (!accepter({Tk::Virgule})) break;
                    }
                }
                exiger(Tk::RParen, "')' de appel");
                g = n;
                continue;
            }
            if (est({Tk::LBracket})) {
                Token tok = suivant();
                if (est({Tk::DeuxPts})) {
                    /* tranche s[:b] → sous_chaine(s, 0, b) */
                    suivant();
                    PE debut = neo(NK::Litteral, tok.ligne); debut->texte = "0";
                    PE fin;
                    if (est({Tk::RBracket})) fin = neo(NK::Litteral, tok.ligne);
                    else { fin = expression(); }
                    /* si fin absente → longueur(s) */
                    PE appel = neo(NK::Appel, tok.ligne); appel->texte = "sous_chaine";
                    appel->args.push_back(g);
                    appel->args.push_back(debut);
                    if (fin->texte.empty() && fin->k == NK::Litteral) {
                        PE lng = neo(NK::Appel, tok.ligne); lng->texte = "longueur"; lng->args.push_back(g);
                        appel->args.push_back(lng);
                    } else {
                        appel->args.push_back(fin);
                    }
                    exiger(Tk::RBracket, "']' de tranche");
                    g = appel;
                    continue;
                }
                PE idx = expression();
                if (est({Tk::DeuxPts})) {
                    /* tranche s[a:b] ou s[a:] */
                    suivant();
                    PE debut = idx;
                    PE fin;
                    if (est({Tk::RBracket})) {
                        PE lng = neo(NK::Appel, tok.ligne); lng->texte = "longueur"; lng->args.push_back(g);
                        fin = lng;
                    } else {
                        fin = expression();
                    }
                    PE appel = neo(NK::Appel, tok.ligne); appel->texte = "sous_chaine";
                    appel->args.push_back(g);
                    appel->args.push_back(debut);
                    appel->args.push_back(fin);
                    exiger(Tk::RBracket, "']' de tranche");
                    g = appel;
                    continue;
                }
                PE n = neo(NK::Index, tok.ligne);
                n->a = g; n->b = idx;
                exiger(Tk::RBracket, "']' d'index");
                g = n;
                continue;
            }
            return g;
        }
    }
    PE atome() {
        const Token &t = voir();
        if (t.type == Tk::Nombre) {
            suivant();
            PE n = neo(NK::Litteral, t.ligne); n->texte = t.texte;
            return n;
        }
        if (t.type == Tk::Flottant) {
            suivant();
            PE n = neo(NK::Litteral, t.ligne); n->texte = t.texte; n->flag = true;
            return n;
        }
        if (t.type == Tk::Chaine) {
            suivant();
            PE n = neo(NK::ChaineLit, t.ligne); n->texte = t.texte;
            return n;
        }
        if (t.type == Tk::LBracket) {
            Token tok = suivant();
            PE n = neo(NK::ListeLit, tok.ligne);
            if (!est({Tk::RBracket})) {
                for (;;) {
                    n->args.push_back(expression());
                    if (!accepter({Tk::Virgule})) break;
                }
            }
            exiger(Tk::RBracket, "']' de liste");
            return n;
        }
        if (t.type == Tk::LParen) {
            suivant();
            PE e = expression();
            exiger(Tk::RParen, "')'");
            return e;
        }
        if (t.type == Tk::Ident || t.type == Tk::Mot) {
            if (t.texte == "vrai") { suivant(); PE n = neo(NK::BooleenLit, t.ligne); n->flag = true; return n; }
            if (t.texte == "faux") { suivant(); PE n = neo(NK::BooleenLit, t.ligne); return n; }
            std::string module;
            if (voir(1).type == Tk::DeuxPoints) {
                module = t.texte;
                suivant(); suivant();
            }
            std::string nom = nom_ident();
            if (module.empty()) {
                PE n = neo(NK::Var, t.ligne); n->texte = nom;
                return n;
            }
            PE n = neo(NK::Appel, t.ligne);   /* module::nom — args remplis par postfixe */
            n->module = module; n->texte = nom;
            return n;
        }
        throw ErreurRose("Attente d'une expression, trouvé '" + t.texte + "'", t.ligne, t.colonne);
    }
};

static Programme analyser(const std::string &source) {
    return Parser(source).programmer();
}

/* ══════════ M4.1 — oracle d'arbre syntaxique : « rose ast » ══════════
   Le premier jet du compilateur auto-hébergé (rose/analyseur.rose) doit
   reproduire CETTE sortie à l'octet près — avant vérification, avant
   émission : uniquement l'osserature du parseur.
   Format (déterministe, sans lignes ni colonnes) :
     programme
     fonction <nom>                     ← signatures triées (ordre std::map)
     externe "<dll>" "<son>" "<sym>"    ← fonctions étrangères seulement
     param <nom> <type> ...
     retour <type>                      ← « vide » si aucune flèche
     <instructions du corps, indentées de 2>
     ... puis les instructions de premier niveau, indentées de 0
   Instructions :
     afficher <e> | lire <nom> | affect <nom> <e> | expr <e>
     si <e> / alors / corps / sinon / corps
     tant_que <e> / corps
     pour <nom> <e> <e> / corps
     retour <e> | retour
     bloc / corps
   Expressions en S-expressions :
     (n 2) (f 2.5) (s "t") (b vraie|faux) (v nom) (app [mod::]nom e…)
     (idx a b) (bin <op> a b) (uni <-|non> a) (fact a) (lst e…)
   Sucre MIRROIR (comme le parseur C++) : √x → (bin ^ x (f 0.5)) ;
   s[a:b] → (app sous_chaine s a b) ; s[:b] → (app sous_chaine s (n 0) b) ;
   s[a:] → (app sous_chaine s a (app longueur s)). */
static void ast_expr(const PE &e, std::string &out) {
    if (!e) { out += "(vide)"; return; }
    switch (e->k) {
        case NK::Litteral: out += e->flag ? "(f " + e->texte + ")" : "(n " + e->texte + ")"; break;
        case NK::ChaineLit: out += "(s " + texte_echappe(e->texte) + ")"; break;
        case NK::BooleenLit: out += e->flag ? "(b vraie)" : "(b faux)"; break;
        case NK::ListeLit: {
            out += "(lst";
            for (auto &a : e->args) { out += " "; ast_expr(a, out); }
            out += ")"; break;
        }
        case NK::Var: out += "(v " + e->texte + ")"; break;
        case NK::Appel: {
            out += "(app ";
            if (!e->module.empty()) out += e->module + "::";
            out += e->texte;
            for (auto &a : e->args) { out += " "; ast_expr(a, out); }
            out += ")"; break;
        }
        case NK::Index: out += "(idx "; ast_expr(e->a, out); out += " "; ast_expr(e->b, out); out += ")"; break;
        case NK::Binaire: out += "(bin " + e->texte + " "; ast_expr(e->a, out); out += " "; ast_expr(e->b, out); out += ")"; break;
        case NK::Unaire: out += "(uni " + e->texte + " "; ast_expr(e->a, out); out += ")"; break;
        case NK::Factorielle: out += "(fact "; ast_expr(e->a, out); out += ")"; break;
    }
}
static std::string ast_ind(int n) { return std::string(2 * n, ' '); }
static void ast_instrs(const std::vector<PI> &v, int ind, std::string &out);
static void ast_instr(const PI &i, int ind, std::string &out) {
    std::string p = ast_ind(ind);
    switch (i->k) {
        case IK::Afficher: out += p + "afficher "; ast_expr(i->expr, out); out += "\n"; break;
        case IK::Lire: out += p + "lire " + i->nom + "\n"; break;
        case IK::Affectation: out += p + "affect " + i->nom + " "; ast_expr(i->expr, out); out += "\n"; break;
        case IK::Si:
            out += p + "si "; ast_expr(i->test, out); out += "\n";
            out += p + "alors\n";
            if (i->alors) ast_instrs(i->alors->instrs, ind + 1, out);
            if (i->sinon) { out += p + "sinon\n"; ast_instrs(i->sinon->instrs, ind + 1, out); }
            break;
        case IK::TantQue:
            out += p + "tant_que "; ast_expr(i->test, out); out += "\n";
            if (i->corps) ast_instrs(i->corps->instrs, ind + 1, out);
            break;
        case IK::Pour:
            out += p + "pour " + i->nom + " "; ast_expr(i->debut, out); out += " "; ast_expr(i->fin, out); out += "\n";
            if (i->corps) ast_instrs(i->corps->instrs, ind + 1, out);
            break;
        case IK::Retour:
            out += p + "retour";
            if (i->expr) { out += " "; ast_expr(i->expr, out); }
            out += "\n"; break;
        case IK::Expr: out += p + "expr "; ast_expr(i->expr, out); out += "\n"; break;
        case IK::Bloc:
            out += p + "bloc\n";
            ast_instrs(i->instrs, ind + 1, out);
            break;
    }
}
static void ast_instrs(const std::vector<PI> &v, int ind, std::string &out) {
    for (auto &i : v) ast_instr(i, ind, out);
}
static std::string ast_programme(const Programme &prog) {
    std::string out = "programme\n";
    for (auto &kv : prog.signatures) {
        const Signature &s = *kv.second;
        out += "fonction " + s.nom + "\n";
        if (!s.corps)
            out += "externe \"" + texte_echappe(s.dll) + "\" \"" + texte_echappe(s.dll_son) +
                   "\" \"" + texte_echappe(s.symbole) + "\"\n";
        for (auto &pr : s.params) out += "param " + pr.first + " " + pr.second.nom() + "\n";
        out += "retour " + s.retour.nom() + "\n";
        if (s.corps) ast_instrs(s.corps->instrs, 1, out);
    }
    ast_instrs(prog.instructions, 0, out);
    return out;
}

/* ── vérificateur de types (port fidèle du vérificateur v2) ───────── */
static const int RANG_VIDE = -1;
static int rang(Ty t) {
    switch (t.n) {
        case T_BOOLEEN: return 0; case T_ENTIER: return 1;
        case T_RATIONNEL: return 2; case T_FLOTTANT: return 3;
    }
    return RANG_VIDE;
}
static bool est_numerique(Ty t) {
    return t.n == T_ENTIER || t.n == T_RATIONNEL || t.n == T_FLOTTANT;
}
static Ty promotion(Ty a, Ty b) {
    int r = std::max(rang(a), rang(b));
    switch (r) {
        case 0: case 1: return Ty(T_ENTIER);
        case 2: return Ty(T_RATIONNEL);
        default: return Ty(T_FLOTTANT);
    }
}

struct EntreeStd { const char *nom; std::vector<std::string> args; std::string retour; };
static const EntreeStd STDLIB[] = {
    {"afficher", {"n"}, "vide"},
    {"saisir_entier", {}, "entier"},
    {"saisir_flottant", {}, "flottant"},
    {"factorielle", {"entier"}, "entier"},
    {"fibonacci", {"entier"}, "entier"},
    {"pgcd", {"entier", "entier"}, "entier"},
    {"ppcm", {"entier", "entier"}, "entier"},
    {"puissance", {"num", "entier"}, "entier"},
    {"est_premier", {"entier"}, "booleen"},
    {"compter_premiers", {"entier"}, "entier"},
    {"nb_chiffres", {"entier"}, "entier"},
    {"nombre_pi", {"entier"}, "chaine"},
    {"longueur", {"liste_ou_chaine_ou_table"}, "entier"},
    {"table", {}, "table"},
    {"inserer", {"table", "tout", "tout"}, "vide"},
    {"cle_existe", {"table", "tout"}, "booleen"},
    {"supprimer", {"table", "tout"}, "booleen"},
    {"cles", {"table"}, "MIXTE"},
    {"valeurs", {"table"}, "MIXTE"},
    {"joindre", {"liste", "liste_elt"}, "vide"},
    {"ordre", {"chaine"}, "entier"},
    {"caractere", {"entier"}, "chaine"},
    {"sous_chaine", {"chaine", "entier", "entier"}, "chaine"},
    {"concatener", {"liste"}, "chaine"},
    {"lire_texte", {"chaine"}, "chaine"},
    {"ecrire_texte", {"chaine", "chaine"}, "vide"},
    {"nb_arguments", {}, "entier"},
    {"argument", {"entier"}, "chaine"},
    {"temps", {}, "entier"},
    {"duree", {"entier"}, "chaine"},
    {"afficher_erreur", {"tout"}, "vide"},
    {"quitter", {"entier"}, "vide"},
    {"abs", {"num"}, "MIXTE"},
    {"min", {"num", "num"}, "NUM_MAX"},
    {"max", {"num", "num"}, "NUM_MAX"},
    {"partie_entiere", {"num"}, "entier"},
    {"fraction", {"entier", "entier"}, "rationnel"},
    /* aléatoire reproductible et tampons d'octets */
    {"graine", {"entier"}, "vide"},
    {"hasard", {"entier"}, "entier"},
    {"tampon", {"entier"}, "tampon"},
    {"octets", {"chaine"}, "tampon"},
    {"tampon_case", {"tampon", "entier"}, "entier"},
    {"tampon_pose", {"tampon", "entier", "entier"}, "vide"},
    {"remplir_hasard", {"tampon", "tampon"}, "vide"},
    {"millisecondes", {}, "entier"},
};
static const EntreeStd DYNAMIQUE[] = {
    {"charger", {"chaine"}, "entier"},
    {"fermer", {"entier"}, "vide"},
    {"appeler0", {"entier", "chaine"}, "flottant"},
    {"appeler1", {"entier", "chaine", "num"}, "flottant"},
    {"appeler2", {"entier", "chaine", "num", "num"}, "flottant"},
};
static const EntreeStd *chercher(const EntreeStd *t, size_t n, const std::string &nom) {
    for (size_t i = 0; i < n; i++) if (nom == t[i].nom) return &t[i];
    return nullptr;
}
static Ty ty_du_nom(const std::string &s) {
    if (s == "entier") return Ty(T_ENTIER);
    if (s == "rationnel") return Ty(T_RATIONNEL);
    if (s == "flottant") return Ty(T_FLOTTANT);
    if (s == "booleen") return Ty(T_BOOLEEN);
    if (s == "chaine") return Ty(T_CHAINE);
    if (s == "table") return Ty(T_TABLE);
    if (s == "tampon") return Ty(T_TAMPON);
    return Ty(T_VIDE);
}

using Env = std::unordered_map<std::string, Ty>;

/* Les temporaires internes du moteur portent la forme « t<chiffres> »
   (%t0, %t1…) dans l'espace des variables RIR : un utilisateur ne peut pas
   déclarer un tel nom, sinon variable et temporaire partageraient le même
   emplacement (corruption mémoire). */
static void verifier_nom_reserve(const std::string &nom, int ligne) {
    if (nom.size() > 1 && nom[0] == 't') {
        bool chiffres = true;
        for (size_t k = 1; k < nom.size(); k++)
            if (!std::isdigit((unsigned char)nom[k])) { chiffres = false; break; }
        if (chiffres)
            throw ErreurRose("Le nom '" + nom + "' est réservé par le moteur", ligne);
    }
}

struct Verificateur {
    Programme &prog;
    Env globales, locals;
    Signature *fonction_en_cours = nullptr;
    Ty assume_retour;
    bool a_assume = false;

    explicit Verificateur(Programme &p) : prog(p) {}

    void verifier() {
        for (auto &kv : prog.signatures) verifier_signature(*kv.second);
        fonction_en_cours = nullptr;
        locals.clear();
        a_assume = false;
        for (PI &i : prog.instructions) instruction(i, globales);
    }

    void verifier_signature(Signature &sig) {
        if (!sig.corps) {                           /* externe */
            std::vector<Ty> frontieres;
            for (auto &pt : sig.params) frontieres.push_back(pt.second);
            frontieres.push_back(sig.retour);
            for (Ty t : frontieres)
                if (t.n != T_ENTIER && t.n != T_FLOTTANT && t.n != T_BOOLEEN && t.n != T_CHAINE)
                    throw ErreurRose("Type " + t.nom() + " non supporté aux frontières d'une "
                                     "fonction externe (utilisez entier/flottant/booleen/chaine)",
                                     sig.ligne);
            return;
        }
        fonction_en_cours = &sig;
        locals.clear();
        for (auto &pt : sig.params) { verifier_nom_reserve(pt.first, 0); locals[pt.first] = pt.second; }
        assume_retour = sig.retour;
        a_assume = true;
        bloc(sig.corps);
        fonction_en_cours = nullptr;
        a_assume = false;
    }

    void bloc(PB b) { for (PI &i : b->instrs) instruction(i, locals); }

    void exiger_booleen(Ty t, int ligne, const std::string &quoi) {
        if (t.n != T_BOOLEEN)
            throw ErreurRose("La condition de " + quoi + " doit être booléenne, trouvé " + t.nom(), ligne);
    }

    void instruction(PI instr, Env &env) {
        switch (instr->k) {
            case IK::Afficher: {
                Ty t = expression(instr->expr, env);
                if (t.n == T_VIDE) throw ErreurRose("Rien à afficher ici", instr->ligne);
                return;
            }
            case IK::Lire: {
                auto it = env.find(instr->nom);
                if (it == env.end()) {
                    verifier_nom_reserve(instr->nom, instr->ligne);
                    env[instr->nom] = Ty(T_ENTIER);
                    instr->type_read = T_ENTIER;
                } else {
                    if (it->second.n != T_ENTIER && it->second.n != T_FLOTTANT
                            && it->second.n != T_RATIONNEL)
                        throw ErreurRose("Ne peut pas lire dans une variable " + it->second.nom()
                                         + " (entier/flottant attendus)", instr->ligne);
                    instr->type_read = it->second.n;
                }
                return;
            }
            case IK::Affectation: {
                Ty t = expression(instr->expr, env);
                if (t.n == T_VIDE)
                    throw ErreurRose("Impossible d'affecter une valeur sans type", instr->ligne);
                Ty cible = t;
                auto it = env.find(instr->nom);
                Ty *existante = nullptr;
                if (it != env.end()) existante = &it->second;
                else {
                    auto g = globales.find(instr->nom);
                    if (g != globales.end()) existante = &g->second;
                }
                if (existante) {
                    cible = *existante;
                    if (cible.n == T_VALEUR || t.n == T_VALEUR) { /* valeur dynamique : aucun contrôle */ }
                    else if (t.n == T_ENTIER && cible.n == T_RATIONNEL) { /* acceptable */ }
                    else if (t != cible) {
                        if (promotion(t, cible) == cible && est_numerique(t) && est_numerique(cible)) {
                        } else
                            throw ErreurRose("'" + instr->nom + "' est " + existante->nom()
                                             + ", on ne peut pas y stocker du " + t.nom(), instr->ligne);
                    }
                }
                verifier_nom_reserve(instr->nom, instr->ligne);
                env[instr->nom] = cible;
                return;
            }
            case IK::Si:
                exiger_booleen(expression(instr->test, env), instr->ligne, "si");
                bloc(instr->alors);
                if (instr->sinon) bloc(instr->sinon);
                return;
            case IK::TantQue:
                exiger_booleen(expression(instr->test, env), instr->ligne, "tant_que");
                bloc(instr->corps);
                return;
            case IK::Pour: {
                if (env.count(instr->nom))
                    throw ErreurRose("La variable de boucle '" + instr->nom + "' existe déjà", instr->ligne);
                Ty td = expression(instr->debut, env);
                Ty tf = expression(instr->fin, env);
                if (td.n != T_ENTIER || tf.n != T_ENTIER)
                    throw ErreurRose("pour…de…vers exige des bornes entières", instr->ligne);
                verifier_nom_reserve(instr->nom, instr->ligne);
                env[instr->nom] = Ty(T_ENTIER);
                bloc(instr->corps);
                return;
            }
            case IK::Retour: {
                if (!a_assume) throw ErreurRose("'retour' hors de fonction", instr->ligne);
                if (!instr->expr) {
                    if (assume_retour.n != T_VIDE)
                        throw ErreurRose("La fonction doit retourner un " + assume_retour.nom(), instr->ligne);
                } else {
                    Ty t = expression(instr->expr, env);
                    if (t != assume_retour
                            && !(est_numerique(t) && est_numerique(assume_retour)
                                 && promotion(t, assume_retour) == assume_retour))
                        throw ErreurRose("La fonction retourne " + t.nom() + ", attendu "
                                         + assume_retour.nom(), instr->ligne);
                }
                return;
            }
            case IK::Expr:
                expression(instr->expr, env);
                return;
            case IK::Bloc:
                for (PI &i : instr->instrs) instruction(i, env);
                return;
        }
    }

    Ty expression(PE e, Env &env) {
        Ty t = _expression(e, env);
        e->type = t.n;
        if (t.n == T_LISTE) e->elt_type = t.elt;
        return t;
    }

    Ty _expression(PE e, Env &env) {
        switch (e->k) {
            case NK::Litteral:
                if (!e->flag && e->texte.find('/') != std::string::npos) return Ty(T_RATIONNEL);
                return Ty(e->flag ? T_FLOTTANT : T_ENTIER);
            case NK::ChaineLit: return Ty(T_CHAINE);
            case NK::BooleenLit: return Ty(T_BOOLEEN);
            case NK::Var: {
                auto it = env.find(e->texte);
                if (it == env.end()) {
                    auto g = globales.find(e->texte);
                    if (g == globales.end())
                        throw ErreurRose("Variable '" + e->texte + "' utilisée sans valeur", e->ligne);
                    return g->second;
                }
                return it->second;
            }
            case NK::ListeLit: {
                if (e->args.empty())
                    throw ErreurRose("Liste vide : le type d'élément est introuvable", e->ligne);
                Ty t_elt = expression(e->args[0], env);
                for (size_t i = 1; i < e->args.size(); i++) {
                    Ty t2 = expression(e->args[i], env);
                    if (t2 != t_elt) {
                        if (est_numerique(t2) && est_numerique(t_elt)) t_elt = promotion(t2, t_elt);
                        else throw ErreurRose("Liste hétérogène : " + t_elt.nom() + " puis " + t2.nom(),
                                             e->ligne);
                    }
                }
                if (t_elt.n == T_LISTE)
                    throw ErreurRose("Listes de listes non supportées pour l'instant", e->ligne);
                return Ty::liste(t_elt.n);
            }
            case NK::Index: {
                Ty tb = expression(e->a, env);
                Ty ti = expression(e->b, env);
                if (tb.n != T_LISTE && tb.n != T_CHAINE && tb.n != T_TABLE && tb.n != T_VALEUR)
                    throw ErreurRose("Index sur un " + tb.nom() + ", une liste est attendue", e->ligne);
                if (tb.n == T_TABLE || tb.n == T_VALEUR) {
                    if (tb.n == T_TABLE && ti.n != T_ENTIER && ti.n != T_RATIONNEL && ti.n != T_FLOTTANT
                            && ti.n != T_CHAINE && ti.n != T_BOOLEEN)
                        throw ErreurRose("Index d'une table doit être un texte ou un nombre", e->ligne);
                    return Ty(T_VALEUR);
                }
                if (ti.n != T_ENTIER)
                    throw ErreurRose(tb.n == T_CHAINE ? "Index d'une chaîne doit être entier"
                                                      : "Index d'une liste doit être entier", e->ligne);
                return tb.n == T_CHAINE ? Ty(T_CHAINE) : Ty(tb.elt);
            }
            case NK::Unaire: {
                Ty t = expression(e->a, env);
                if (e->texte == "non") {
                    exiger_booleen(t, e->ligne, "non");
                    return Ty(T_BOOLEEN);
                }
                if (!est_numerique(t))
                    throw ErreurRose("Unaire " + e->texte + " sur un " + t.nom(), e->ligne);
                return t;
            }
            case NK::Factorielle: {
                Ty t = expression(e->a, env);
                if (t.n != T_ENTIER)
                    throw ErreurRose("Factorielle attend un entier, trouvé " + t.nom(), e->ligne);
                return Ty(T_ENTIER);
            }
            case NK::Binaire: return binaire(e, env);
            case NK::Appel: return appel(e, env);
        }
        throw ErreurRose("Expression inconnue", e->ligne);
    }

    bool droite_unaire_moins(const PE &e) {
        return e->k == NK::Unaire && e->texte == "-";
    }

    Ty binaire(PE e, Env &env) {
        const std::string &op = e->texte;
        Ty tg = expression(e->a, env);
        Ty td = expression(e->b, env);
        if (op == "et" || op == "ou") {
            exiger_booleen(tg, e->ligne, "et/ou");
            exiger_booleen(td, e->ligne, "et/ou");
            return Ty(T_BOOLEEN);
        }
        if (op == "==" || op == "!=" || op == "≈" || op == "?=" || op == "=~") {
            if (tg == td || (est_numerique(tg) && est_numerique(td))) return Ty(T_BOOLEEN);
            throw ErreurRose("Comparaison " + tg.nom() + " " + op + " " + td.nom() + " impossible",
                             e->ligne);
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if ((est_numerique(tg) && est_numerique(td)) || (tg.n == T_CHAINE && td.n == T_CHAINE))
                return Ty(T_BOOLEEN);
            throw ErreurRose("Ordre " + tg.nom() + " " + op + " " + td.nom() + " impossible", e->ligne);
        }
        if (op == "+") {
            if (tg.n == T_CHAINE && td.n == T_CHAINE) return Ty(T_CHAINE);
            if (est_numerique(tg) && est_numerique(td)) return promotion(tg, td);
            throw ErreurRose("Addition " + tg.nom() + " + " + td.nom() + " impossible", e->ligne);
        }
        if (op == "-" || op == "*") {
            if (est_numerique(tg) && est_numerique(td)) return promotion(tg, td);
            throw ErreurRose("Opération " + tg.nom() + " " + op + " " + td.nom() + " impossible", e->ligne);
        }
        if (op == "/") {
            if (est_numerique(tg) && est_numerique(td)) {
                Ty r = promotion(tg, td);
                if (r.n == T_ENTIER) return Ty(T_RATIONNEL);     /* division exacte */
                return r;
            }
            throw ErreurRose("Division " + tg.nom() + " / " + td.nom() + " impossible", e->ligne);
        }
        if (op == "%") {
            if (tg.n == T_ENTIER && td.n == T_ENTIER) return Ty(T_ENTIER);
            throw ErreurRose("Le modulo exige deux entiers", e->ligne);
        }
        if (op == "div") {
            if (!(est_numerique(tg) || tg.n == T_BOOLEEN))
                throw ErreurRose("div attend un nombre à gauche, trouvé " + tg.nom(), e->ligne);
            if (!(est_numerique(td) || td.n == T_BOOLEEN))
                throw ErreurRose("div attend un nombre à droite, trouvé " + td.nom(), e->ligne);
            if (td.n == T_FLOTTANT || tg.n == T_FLOTTANT) return Ty(T_FLOTTANT);
            return Ty(T_ENTIER);
        }
        if (op == "^") {
            if (!est_numerique(tg))
                throw ErreurRose("Puissance sur une base " + tg.nom(), e->ligne);
            if (td.n == T_FLOTTANT) return Ty(T_FLOTTANT);
            if (td.n == T_ENTIER || td.n == T_RATIONNEL || td.n == T_BOOLEEN) {
                if (droite_unaire_moins(e->b)) {
                    if (tg.n == T_ENTIER) return Ty(T_RATIONNEL);
                    return tg;
                }
                return tg.n == T_ENTIER ? Ty(T_ENTIER) : tg;
            }
            throw ErreurRose("Exposant " + td.nom() + " impossible", e->ligne);
        }
        throw ErreurRose("Opérateur '" + op + "' inconnu", e->ligne);
    }

    void verifier_args(PE e, const EntreeStd &sig, Env &env, const std::string &nom) {
        if (e->args.size() != sig.args.size())
            throw ErreurRose(nom + " attend " + std::to_string(sig.args.size())
                             + " argument(s), reçu " + std::to_string(e->args.size()), e->ligne);
        Ty t_elt = Ty(T_VIDE);
        bool a_elt = false;
        for (size_t i = 0; i < e->args.size(); i++) {
            Ty ta = expression(e->args[i], env);
            const std::string &t_att = sig.args[i];
            if (t_att == "chaine") {
                if (ta.n != T_CHAINE) throw ErreurRose(nom + ": une chaine est attendue", e->ligne);
            } else if (t_att == "num") {
                if (!est_numerique(ta))
                    throw ErreurRose(nom + ": un nombre est attendu, reçu " + ta.nom(), e->ligne);
            } else if (t_att == "entier") {
                if (ta.n != T_ENTIER)
                    throw ErreurRose(nom + ": un entier est attendu, reçu " + ta.nom(), e->ligne);
            } else if (t_att == "liste") {
                if (ta.n != T_LISTE)
                    throw ErreurRose(nom + ": une liste est attendue, reçu " + ta.nom(), e->ligne);
                t_elt = Ty(ta.elt); a_elt = true;   // type de l'ÉLÉMENT, pas de la liste
            } else if (t_att == "tampon") {
                if (ta.n != T_TAMPON)
                    throw ErreurRose(nom + ": un tampon est attendu, reçu " + ta.nom(), e->ligne);
            } else if (t_att == "liste_ou_chaine" || t_att == "liste_ou_chaine_ou_table") {
                if (ta.n != T_LISTE && ta.n != T_CHAINE && ta.n != T_TABLE && ta.n != T_TAMPON)
                    throw ErreurRose(nom + ": une liste, une chaîne ou une table est attendue, reçu " + ta.nom(), e->ligne);
            } else if (t_att == "liste_elt") {
                if (a_elt && t_elt.n != T_VIDE && ta != t_elt
                        && !(est_numerique(ta) && est_numerique(t_elt)
                             && promotion(ta, t_elt) == t_elt))
                    throw ErreurRose(nom + ": élément " + ta.nom() + " ne correspond pas à la liste "
                                     + t_elt.nom(), e->ligne);
            } else if (t_att == "tout") {
                /* tout type est accepté (afficher_erreur) */
            }
        }
    }

    Ty appel(PE e, Env &env) {
        if (e->module == "dynamique") {
            const EntreeStd *sig =chercher(DYNAMIQUE,
                sizeof DYNAMIQUE / sizeof DYNAMIQUE[0], e->texte);
            if (!sig)
                throw ErreurRose("Fonction dynamique::" + e->texte + " inconnue "
                                 "(disponibles: charger, fermer, appeler0, appeler1, appeler2)",
                                 e->ligne);
            verifier_args(e, *sig, env, "dynamique::" + e->texte);
            return ty_du_nom(sig->retour);
        }
        if ((e->texte == "entier" || e->texte == "rationnel" || e->texte == "flottant"
             || e->texte == "chaine") && e->args.size() == 1) {
            Ty cible = ty_du_nom(e->texte);
            Ty t = expression(e->args[0], env);
            if (cible.n == T_CHAINE) return Ty(T_CHAINE);
            if (t.n == T_CHAINE) {
                if (cible.n == T_ENTIER) return Ty(T_ENTIER);
                if (cible.n == T_FLOTTANT) return Ty(T_FLOTTANT);
                throw ErreurRose("Conversion chaine → rationnel non disponible, passez par flottant",
                                 e->ligne);
            }
            if (!est_numerique(t))
                throw ErreurRose("Conversion " + t.nom() + " → " + cible.nom() + " impossible",
                                 e->ligne);
            return cible;
        }
        auto it = prog.signatures.find(e->texte);
        if (it != prog.signatures.end()) {
            Signature &sig = *it->second;
            std::vector<Ty> attends;
            for (auto &pt : sig.params) attends.push_back(pt.second);
            if (e->args.size() != attends.size())
                throw ErreurRose(sig.nom + " attend " + std::to_string(attends.size())
                                 + " argument(s), reçu " + std::to_string(e->args.size()), e->ligne);
            for (size_t i = 0; i < attends.size(); i++) {
                Ty ta = expression(e->args[i], env);
                Ty t_att = attends[i];
                if (ta != t_att && !(est_numerique(ta) && est_numerique(t_att)
                                     && promotion(ta, t_att) == t_att))
                    throw ErreurRose("Argument de " + sig.nom + ": " + ta.nom() + " reçu, "
                                     + t_att.nom() + " attendu", e->ligne);
            }
            return sig.retour;
        }
        const EntreeStd *sd = chercher(STDLIB, sizeof STDLIB / sizeof STDLIB[0], e->texte);
        if (!sd) throw ErreurRose("Fonction '" + e->texte + "' inconnue", e->ligne);
        verifier_args(e, *sd, env, e->texte);
        if (sd->retour == "MIXTE") {
            Ty t = expression(e->args[0], env);
            return t.n == T_BOOLEEN ? Ty(T_ENTIER) : t;
        }
        if (sd->retour == "NUM_MAX") {
            Ty t1 = expression(e->args[0], env);
            Ty t2 = expression(e->args[1], env);
            return promotion(t1, t2);
        }
        return ty_du_nom(sd->retour);
    }
};

/* ── formatteuse de flottants « aller le plus court fidèle » ──────── */
static std::string texte_flottant(double d) {
    char buf[64];
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*g", p, d);
        if (strtod(buf, nullptr) == d) {
            std::string s(buf);
            if (s.find('.') == std::string::npos && s.find('e') == std::string::npos
                    && s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
                s += ".0";
            return s;
        }
    }
    snprintf(buf, sizeof buf, "%.17g", d);
    return std::string(buf);
}

static int sgn_q(const mpq_class &x) { return mpq_sgn(x.get_mpq_t()); }

/* ── repli de constantes exactes ──────────────────────────────────── */
static mpz_class fact_arbre(unsigned long i, unsigned long j) {
    if (i > j) return mpz_class(1);
    if (i == j) return mpz_class(i);
    unsigned long m = i + (j - i) / 2;
    return fact_arbre(i, m) * fact_arbre(m + 1, j);
}

static bool lit_est_entier(const E &e) { return !e.flag && e.texte.find('/') == std::string::npos; }

static mpq_class lit_valeur_q(const E &e) {
    if (e.flag) {
        mpq_class q;
        mpq_set_d(q.get_mpq_t(), std::strtod(e.texte.c_str(), nullptr));
        return q;
    }
    return mpq_class(e.texte);
}
static double lit_valeur_d(const E &e) {
    /* littéral « p/q » replié (opération mixte avec un flottant) : strtod
       s'arrêterait à '/' → diviser par le dénominateur, comme vers_d. */
    double a = std::strtod(e.texte.c_str(), nullptr);
    size_t b = e.texte.find('/');
    if (b != std::string::npos) a /= std::strtod(e.texte.c_str() + b + 1, nullptr);
    return a;
}

static PE replier(PE e);

static PE litt_depuis_q(const mpq_class &r, int ligne) {
    mpq_class q = r;
    q.canonicalize();
    PE n = neo(NK::Litteral, ligne);
    if (q.get_den() == 1) { n->texte = q.get_num().get_str(); n->type = T_ENTIER; }
    else { n->texte = q.get_num().get_str() + "/" + q.get_den().get_str(); n->type = T_RATIONNEL; }
    return n;
}

static PE litt_depuis_z(const mpz_class &r, int ligne) {
    PE n = neo(NK::Litteral, ligne);
    n->texte = r.get_str(); n->type = T_ENTIER;
    return n;
}

static PE fold_bin(const std::string &op, const E &g, const E &d, int ligne) {
    if (g.flag || d.flag) {
        double a = lit_valeur_d(g), b = lit_valeur_d(d), r;
        if (op == "+") r = a + b;
        else if (op == "-") r = a - b;
        else if (op == "*") r = a * b;
        else if (op == "/") { if (b == 0.0) return nullptr; r = a / b; }
        else if (op == "^") r = std::pow(a, b);
        else return nullptr;
        if (std::isnan(r) || std::isinf(r)) return nullptr;   /* Python lèverait une erreur */
        PE n = neo(NK::Litteral, ligne); n->texte = texte_flottant(r); n->flag = true; n->type = T_FLOTTANT;
        return n;
    }
    if (op == "≈" || op == "?=" || op == "=~" || op == "==" || op == "!=" || op == "%")
        return nullptr;
    mpq_class a = lit_valeur_q(g), b = lit_valeur_q(d);
    if (op == "+") return litt_depuis_q(a + b, ligne);
    if (op == "-") return litt_depuis_q(a - b, ligne);
    if (op == "*") return litt_depuis_q(a * b, ligne);
    if (op == "/") {
        if (sgn_q(b) == 0) throw ErreurRose("Division par zéro dans une constante", ligne);
        return litt_depuis_q(a / b, ligne);
    }
    if (op == "^") {
        if (b.get_den() != 1) return nullptr;
        mpz_class expo = b.get_num();
        if (mpz_sgn(expo.get_mpz_t()) < 0 && a == 0)
            throw ErreurRose("0^négatif dans une constante", ligne);
        if (!mpz_fits_slong_p(expo.get_mpz_t())) return nullptr;
        long e = mpz_get_si(expo.get_mpz_t());
        mpz_class num, den;
        if (e >= 0) {
            mpz_pow_ui(num.get_mpz_t(), a.get_num().get_mpz_t(), (unsigned long)e);
            mpz_pow_ui(den.get_mpz_t(), a.get_den().get_mpz_t(), (unsigned long)e);
        } else {
            unsigned long k = (unsigned long)(-e);
            mpz_pow_ui(num.get_mpz_t(), a.get_den().get_mpz_t(), k);
            mpz_pow_ui(den.get_mpz_t(), a.get_num().get_mpz_t(), k);
        }
        return litt_depuis_q(mpq_class(num, den), ligne);
    }
    if (op == "div") {
        if (sgn_q(b) == 0) throw ErreurRose("division par zéro dans une constante", ligne);
        mpq_class q = a / b;
        mpz_class f;
        mpz_fdiv_q(f.get_mpz_t(), q.get_num().get_mpz_t(), q.get_den().get_mpz_t());
        return litt_depuis_z(f, ligne);
    }
    return nullptr;
}

static bool est_litt(const PE &e) { return e && e->k == NK::Litteral; }

static PE replier(PE e) {
    for (PE &s : e->args) s = replier(s);
    if (e->a) e->a = replier(e->a);
    if (e->b) e->b = replier(e->b);
    if (e->k == NK::Binaire && est_litt(e->a) && est_litt(e->b)) {
        PE r = fold_bin(e->texte, *e->a, *e->b, e->ligne);
        if (r) return r;
    }
    if (e->k == NK::Unaire && e->texte == "-" && est_litt(e->a)) {
        if (e->a->flag) {
            PE n = neo(NK::Litteral, e->ligne);
            n->texte = texte_flottant(-lit_valeur_d(*e->a)); n->flag = true; n->type = T_FLOTTANT;
            return n;
        }
        return litt_depuis_q(-lit_valeur_q(*e->a), e->ligne);
    }
    if (e->k == NK::Factorielle && est_litt(e->a) && !e->a->flag && lit_est_entier(*e->a)) {
        mpz_class n;
        if (mpz_set_str(n.get_mpz_t(), e->a->texte.c_str(), 10) == 0
                && mpz_sgn(n.get_mpz_t()) >= 0 && mpz_cmp_ui(n.get_mpz_t(), 20000) <= 0) {
            if (mpz_cmp_ui(n.get_mpz_t(), 2) < 0) return litt_depuis_z(mpz_class(1), e->ligne);
            mpz_class num, den;
            mpz_class part = fact_arbre(2, mpz_get_ui(n.get_mpz_t()));
            return litt_depuis_z(part, e->ligne);
        }
    }
    return e;
}

static void replier_instr(PI i);

static void replier_bloc_in(PB b) { for (PI &i : b->instrs) replier_instr(i); }

static void replier_instr(PI i) {
    switch (i->k) {
        case IK::Afficher: case IK::Expr: i->expr = replier(i->expr); return;
        case IK::Affectation: i->expr = replier(i->expr); return;
        case IK::Retour: if (i->expr) i->expr = replier(i->expr); return;
        case IK::Si:
            i->test = replier(i->test);
            replier_bloc_in(i->alors);
            if (i->sinon) replier_bloc_in(i->sinon);
            return;
        case IK::TantQue:
            i->test = replier(i->test);
            replier_bloc_in(i->corps);
            return;
        case IK::Pour:
            i->debut = replier(i->debut);
            i->fin = replier(i->fin);
            replier_bloc_in(i->corps);
            return;
        case IK::Bloc: for (PI &s : i->instrs) replier_instr(s); return;
        case IK::Lire: return;
    }
}

static void replier_constants(Programme &prog) {
    for (auto &kv : prog.signatures)
        if (kv.second->corps) replier_bloc_in(kv.second->corps);
    for (PI &i : prog.instructions) replier_instr(i);
}

/* ── valeurs ──────────────────────────────────────────────────────── */
struct Valeur {
    int ty = T_VIDE;
    mpz_class z;
    mpq_class q;
    double f = 0.0;
    bool b = false;
    std::string s;
    std::shared_ptr<std::vector<Valeur>> l;
    std::shared_ptr<std::vector<std::pair<Valeur,Valeur>>> t;
    std::shared_ptr<std::string> o;   /* buffer d'octets contigu (T_TAMPON) */

    static Valeur de_z(mpz_class v)  { Valeur x; x.ty = T_ENTIER;   x.z = std::move(v); return x; }
    static Valeur de_q(mpq_class v)  { Valeur x; x.ty = T_RATIONNEL;x.q = std::move(v); x.q.canonicalize(); return x; }
    static Valeur de_f(double v)     { Valeur x; x.ty = T_FLOTTANT; x.f = v; return x; }
    static Valeur de_b(bool v)       { Valeur x; x.ty = T_BOOLEEN;  x.b = v; return x; }
    static Valeur de_s(std::string v){ Valeur x; x.ty = T_CHAINE;   x.s = std::move(v); return x; }
    static Valeur de_l(std::shared_ptr<std::vector<Valeur>> v) {
        Valeur x; x.ty = T_LISTE; x.l = std::move(v); return x;
    }
    static Valeur de_t(std::shared_ptr<std::vector<std::pair<Valeur,Valeur>>> v) {
        Valeur x; x.ty = T_TABLE; x.t = std::move(v); return x;
    }
    static Valeur de_o(std::shared_ptr<std::string> v) {
        Valeur x; x.ty = T_TAMPON; x.o = std::move(v); return x;
    }
    static Valeur vide() { return Valeur(); }
};
/* Contrat de layout : ty@0 et z@+8 (arithmétique inline M2.15, hypothèses du
   JIT). La taille TOTALE dépend de la stdlib C++ (libstdc++ std::string=32 sous
   Linux ; libc++=24 sous windows-gnu via zig) : on ne l'épinglette que sur le
   terrain natif, là où rir.h est réellement consommé. */
static_assert(offsetof(Valeur, ty) == 0 && offsetof(Valeur, z) == 8,
              "offsets Valeur modifiés : synchroniser arithmétique inline/JIT");
#if defined(__LP64__) && !defined(_WIN32)
static_assert(sizeof(Valeur) == 152,
              "layout Valeur modifié : synchroniser les DEUX définitions (rose.cpp et rir.h)");
#endif

static void erreur_runtime(const std::string &msg) {
    std::fflush(stdout);
    fprintf(stderr, "Rose : erreur — %s\n", msg.c_str());
    exit(1);
}

/* ── conversions ──────────────────────────────────────────────────── */
static mpz_class vers_z(const Valeur &v) {
    switch (v.ty) {
        case T_ENTIER: return v.z;
        case T_BOOLEEN: return mpz_class(v.b ? 1 : 0);
        case T_RATIONNEL: { mpz_class r; mpz_fdiv_q(r.get_mpz_t(), v.q.get_num().get_mpz_t(),
                                                    v.q.get_den().get_mpz_t()); return r; }
        case T_FLOTTANT: { mpz_class r; mpz_set_d(r.get_mpz_t(), v.f); return r; }
    }
    erreur_runtime("conversion vers entier impossible");
    return mpz_class();
}
static mpq_class vers_q(const Valeur &v) {
    switch (v.ty) {
        case T_ENTIER: return mpq_class(v.z);
        case T_BOOLEEN: return mpq_class(v.b ? 1 : 0);
        case T_RATIONNEL: return v.q;
        case T_FLOTTANT: {
            if (std::isnan(v.f) || std::isinf(v.f))
                erreur_runtime("flottant infini/non-nombre sans rationnel correspondant");
            mpq_class r; mpq_set_d(r.get_mpq_t(), v.f); return r;
        }
    }
    erreur_runtime("conversion vers rationnel impossible");
    return mpq_class();
}
static double vers_d(const Valeur &v) {
    switch (v.ty) {
        case T_FLOTTANT: return v.f;
        case T_ENTIER: return mpz_get_d(v.z.get_mpz_t());
        case T_RATIONNEL: return mpq_get_d(v.q.get_mpq_t());
        case T_BOOLEEN: return v.b ? 1.0 : 0.0;
    }
    erreur_runtime("conversion vers flottant impossible");
    return 0.0;
}

static void chaine_vers_entier(mpz_class &dest, const std::string &ch) {
    if (mpz_set_str(dest.get_mpz_t(), ch.c_str(), 10) == 0) return;
    size_t p = ch.find('/');
    if (p != std::string::npos) {
        mpz_class n, d;
        if (mpz_set_str(n.get_mpz_t(), ch.substr(0, p).c_str(), 10)
                || mpz_set_str(d.get_mpz_t(), ch.substr(p + 1).c_str(), 10)
                || mpz_sgn(d.get_mpz_t()) == 0)
            erreur_runtime("conversion chaine → entier impossible");
        mpz_fdiv_q(dest.get_mpz_t(), n.get_mpz_t(), d.get_mpz_t());
        return;
    }
    size_t pt = ch.find('.');
    if (pt != std::string::npos) {
        if (mpz_set_str(dest.get_mpz_t(), ch.substr(0, pt).c_str(), 10) == 0) return;
    }
    erreur_runtime("conversion chaine → entier impossible");
}
static double chaine_vers_flottant(const std::string &ch) {
    char *fin = nullptr;
    double d = std::strtod(ch.c_str(), &fin);
    if (fin == ch.c_str()) {
        if (ch == "infini") return 1.0 / 0.0;
        erreur_runtime("conversion chaine → flottant impossible");
    }
    return d;
}

static std::string chaine_de(const Valeur &v) {
    switch (v.ty) {
        case T_TAMPON: return v.o ? *v.o : std::string();
        case T_ENTIER: return v.z.get_str();
        case T_RATIONNEL: {
            mpq_class q = v.q; q.canonicalize();
            if (q.get_den() == 1) return q.get_num().get_str();
            return q.get_num().get_str() + "/" + q.get_den().get_str();
        }
        case T_FLOTTANT: {
            if (v.f == std::floor(v.f) && std::fabs(v.f) < 1e16) {
                char buf[64]; snprintf(buf, sizeof buf, "%lld", (long long)v.f);
                return std::string(buf);
            }
            return texte_flottant(v.f);
        }
        case T_BOOLEEN: return v.b ? "vrai" : "faux";
        case T_CHAINE: return v.s;
        case T_LISTE: {
            std::string r = "[";
            for (size_t i = 0; i < v.l->size(); i++) {
                if (i) r += ", ";
                r += chaine_de((*v.l)[i]);
            }
            return r + "]";
        }
        case T_TABLE: {
            std::string r = "[";
            for (size_t i = 0; i < v.t->size(); i++) {
                if (i) r += ", ";
                r += chaine_de((*v.t)[i].first) + " => " + chaine_de((*v.t)[i].second);
            }
            return r + "]";
        }
    }
    return "";
}

/* ── écriture exacte (périodes) et affichage ──────────────────────── */
static void ecri(FILE *out, const Valeur &v) {
    switch (v.ty) {
        case T_ENTIER: fputs(v.z.get_str().c_str(), out); return;
        case T_FLOTTANT: {
            double d = v.f;
            if (d != d) { fputs("non_nombre", out); return; }
            if (d == 1.0 / 0.0) { fputs("infini", out); return; }
            if (d == -1.0 / 0.0) { fputs("-infini", out); return; }
            if (d == std::floor(d) && std::fabs(d) < 1e16) {
                char buf[64]; snprintf(buf, sizeof buf, "%lld", (long long)d);
                fputs(buf, out); return;
            }
            fputs(texte_flottant(d).c_str(), out); return;
        }
        case T_BOOLEEN: fputs(v.b ? "vrai" : "faux", out); return;
        case T_CHAINE: fputs(v.s.c_str(), out); return;
        case T_TABLE: { std::string r = chaine_de(v); fputs(r.c_str(), out); return; }
        case T_TAMPON: erreur_runtime("Un tampon ne s'affiche pas directement — utiliser chaine(tampon)");
        case T_RATIONNEL: {
            mpq_class q = v.q; q.canonicalize();
            if (q.get_den() == 1) { fputs(q.get_num().get_str().c_str(), out); return; }
            if (mpz_sizeinbase(q.get_num().get_mpz_t(), 10)
                    + mpz_sizeinbase(q.get_den().get_mpz_t(), 10) > 500) {
                fputs(q.get_num().get_str().c_str(), out); fputc('/', out);
                fputs(q.get_den().get_str().c_str(), out); return;
            }
            mpz_class entier;
            mpz_fdiv_q(entier.get_mpz_t(), q.get_num().get_mpz_t(), q.get_den().get_mpz_t());
            mpz_class reste; mpz_abs(reste.get_mpz_t(), q.get_num().get_mpz_t());
            mpz_mod(reste.get_mpz_t(), reste.get_mpz_t(), q.get_den().get_mpz_t());
            char digits[201];
            int ndig = 0, peri = -1, nrestes = 0;
            mpz_class restes[200]; int pos[200];
            while (mpz_sgn(reste.get_mpz_t()) != 0 && ndig < 200) {
                for (int j = 0; j < nrestes; j++)
                    if (restes[j] == reste) { peri = pos[j]; break; }
                if (peri >= 0) break;
                restes[nrestes] = reste; pos[nrestes++] = ndig;
                mpz_mul_ui(reste.get_mpz_t(), reste.get_mpz_t(), 10);
                mpz_class chiffre;
                mpz_fdiv_q(chiffre.get_mpz_t(), reste.get_mpz_t(), q.get_den().get_mpz_t());
                mpz_mod(reste.get_mpz_t(), reste.get_mpz_t(), q.get_den().get_mpz_t());
                digits[ndig++] = (char)('0' + mpz_get_ui(chiffre.get_mpz_t()));
            }
            fputs(entier.get_str().c_str(), out);
            if (ndig) {
                fputc('.', out);
                fwrite(digits, 1, (size_t)(peri >= 0 ? peri : ndig), out);
                if (peri >= 0) {
                    fputc('[', out);
                    fwrite(digits + peri, 1, (size_t)(ndig - peri), out);
                    fputc(']', out);
                } else if (ndig >= 200) {
                    fputs("…", out);
                }
            }
            return;
        }
        case T_LISTE: {
            fputc('[', out);
            for (size_t i = 0; i < v.l->size(); i++) {
                if (i) fputs(", ", out);
                ecri(out, (*v.l)[i]);
            }
            fputc(']', out); return;
        }
    }
}

static void afficher(const Valeur &v) { ecri(stdout, v); fputc('\n', stdout); }

/* ── lecture stdin ────────────────────────────────────────────────── */
static std::string lire_ligne() {
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) erreur_runtime("entrée attendue, fichier fermé");
    size_t l = strlen(buf);
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r' || buf[l - 1] == ' ')) buf[--l] = 0;
    return std::string(buf, l);
}
static Valeur lire_selon(int type_cible) {
    std::string t = lire_ligne();
    if (type_cible == T_FLOTTANT) {
        char *fin = nullptr;
        double d = std::strtod(t.c_str(), &fin);
        if (fin == t.c_str()) erreur_runtime("flottant invalide");
        return Valeur::de_f(d);
    }
    if (type_cible == T_RATIONNEL) {
        mpq_class q;
        if (mpq_set_str(q.get_mpq_t(), t.c_str(), 10)) erreur_runtime("rationnel invalide");
        q.canonicalize();
        return Valeur::de_q(q);
    }
    /* entier : tolérant — « p/q » tronqué vers zéro, décimale tronquée */
    if (t.find('/') != std::string::npos) {
        mpq_class q;
        if (mpq_set_str(q.get_mpq_t(), t.c_str(), 10) == 0) {
            q.canonicalize();
            mpz_class r;
            mpz_tdiv_q(r.get_mpz_t(), q.get_num().get_mpz_t(), q.get_den().get_mpz_t());
            return Valeur::de_z(r);
        }
    }
    if (t.find('.') != std::string::npos) {
        double d = std::strtod(t.c_str(), nullptr);
        mpz_class r; mpz_set_d(r.get_mpz_t(), d);
        return Valeur::de_z(r);
    }
    mpz_class r;
    if (mpz_set_str(r.get_mpz_t(), t.c_str(), 10)) erreur_runtime("nombre entier invalide");
    return Valeur::de_z(r);
}

/* ── bibliothèque mathématique (port du runtime C/GMP) ────────────── */
static void factorielle_arbre(mpz_class &dest, unsigned long lo, unsigned long hi) {
    if (lo > hi) { dest = 1; return; }
    if (hi - lo < 16) {
        dest = lo;
        for (unsigned long x = lo + 1; x <= hi; x++) mpz_mul_ui(dest.get_mpz_t(), dest.get_mpz_t(), x);
        return;
    }
    unsigned long mid = lo + (hi - lo) / 2;
    mpz_class g, d;
    factorielle_arbre(g, lo, mid);
    factorielle_arbre(d, mid + 1, hi);
    dest = g * d;
}
static mpz_class rose_factorielle(const mpz_class &n) {
    if (mpz_sgn(n.get_mpz_t()) < 0) erreur_runtime("factorielle d'un négatif : impossible");
    if (mpz_cmp_ui(n.get_mpz_t(), 0xFFFFFFFFUL) > 0) erreur_runtime("factorielle trop grande");
    mpz_class r;
    factorielle_arbre(r, 1, mpz_get_ui(n.get_mpz_t()));
    return r;
}
static mpz_class rose_fibonacci(const mpz_class &nn) {
    if (mpz_sgn(nn.get_mpz_t()) < 0) erreur_runtime("fibonacci négatif : impossible");
    mpz_class a = 0, b = 1, t1, t2, t3;
    mpz_class n = nn;
    mp_bitcnt_t bits = mpz_sizeinbase(n.get_mpz_t(), 2);
    for (mp_bitcnt_t i = bits; i-- > 0;) {
        mpz_mul_ui(t1.get_mpz_t(), b.get_mpz_t(), 2);
        mpz_sub(t1.get_mpz_t(), t1.get_mpz_t(), a.get_mpz_t());
        mpz_mul(t1.get_mpz_t(), t1.get_mpz_t(), a.get_mpz_t());
        mpz_mul(t2.get_mpz_t(), a.get_mpz_t(), a.get_mpz_t());
        mpz_mul(t3.get_mpz_t(), b.get_mpz_t(), b.get_mpz_t());
        mpz_add(t2.get_mpz_t(), t2.get_mpz_t(), t3.get_mpz_t());
        if (mpz_tstbit(n.get_mpz_t(), i)) {
            a = t2;
            mpz_add(b.get_mpz_t(), t1.get_mpz_t(), t2.get_mpz_t());
        } else { a = t1; b = t2; }
    }
    return a;
}
static bool rose_est_premier(const mpz_class &n) {
    return mpz_probab_prime_p(n.get_mpz_t(), 25) != 0 && mpz_cmp_ui(n.get_mpz_t(), 1) > 0;
}
static mpz_class rose_nb_chiffres(const mpz_class &n) {
    if (mpz_sgn(n.get_mpz_t()) == 0) return mpz_class(1);
    mpz_class t; mpz_abs(t.get_mpz_t(), n.get_mpz_t());
    return mpz_class((unsigned long)mpz_sizeinbase(t.get_mpz_t(), 10));
}
static mpz_class rose_compter_premiers(const mpz_class &nn) {
    if (mpz_sgn(nn.get_mpz_t()) < 0 || mpz_cmp_ui(nn.get_mpz_t(), 0xFFFFFFFFUL) > 0)
        erreur_runtime("crible trop grand");
    unsigned long n = mpz_get_ui(nn.get_mpz_t());
    std::vector<unsigned char> crible(n + 1, 0);
    unsigned long compte = 0;
    for (unsigned long i = 2; i <= n; i++) {
        if (!crible[i]) {
            compte++;
            if (i <= n / i)
                for (unsigned long j = i * i; j <= n; j += i) crible[j] = 1;
        }
    }
    return mpz_class(compte);
}
static std::string rose_pi(const mpz_class &nn) {
    if (mpz_sgn(nn.get_mpz_t()) <= 0 || mpz_cmp_ui(nn.get_mpz_t(), 2000000) > 0)
        erreur_runtime("décimales de π hors limites (1..2000000)");
    unsigned long chiffres = mpz_get_ui(nn.get_mpz_t());
    unsigned long S = chiffres + 40;
    mpz_class dixS; mpz_ui_pow_ui(dixS.get_mpz_t(), 10, S);
    unsigned long xs[2] = {5, 239};
    mpz_class cibles[2];
    for (int w = 0; w < 2; w++) {
        mpz_class &dest = cibles[w];
        mpz_class x2, p, terme;
        mpz_set_ui(x2.get_mpz_t(), xs[w]); mpz_mul(x2.get_mpz_t(), x2.get_mpz_t(), x2.get_mpz_t());
        mpz_set_ui(p.get_mpz_t(), xs[w]);
        unsigned long k = 1;
        mpz_fdiv_q(terme.get_mpz_t(), dixS.get_mpz_t(), p.get_mpz_t());
        dest = terme;
        int signe = -1;
        for (;;) {
            mpz_mul(p.get_mpz_t(), p.get_mpz_t(), x2.get_mpz_t());
            k += 2;
            mpz_mul_ui(terme.get_mpz_t(), p.get_mpz_t(), k);
            mpz_fdiv_q(terme.get_mpz_t(), dixS.get_mpz_t(), terme.get_mpz_t());
            if (mpz_sgn(terme.get_mpz_t()) == 0) break;
            if (signe < 0) mpz_sub(dest.get_mpz_t(), dest.get_mpz_t(), terme.get_mpz_t());
            else mpz_add(dest.get_mpz_t(), dest.get_mpz_t(), terme.get_mpz_t());
            signe = -signe;
        }
    }
    mpz_class at5 = cibles[0], at239 = cibles[1], pi;
    mpz_mul_ui(at5.get_mpz_t(), at5.get_mpz_t(), 16);
    mpz_mul_ui(at239.get_mpz_t(), at239.get_mpz_t(), 4);
    mpz_sub(pi.get_mpz_t(), at5.get_mpz_t(), at239.get_mpz_t());
    char *brut = mpz_get_str(nullptr, 10, pi.get_mpz_t());
    size_t lbrut = strlen(brut);
    std::string sortie(chiffres + 2, ' ');
    sortie[0] = brut[0]; sortie[1] = '.';
    for (unsigned long i = 0; i < chiffres; i++)
        sortie[2 + i] = (i + 1 < lbrut) ? brut[i + 1] : '0';
    if (1 + chiffres < lbrut && brut[1 + chiffres] >= '5') {
        long i = (long)chiffres + 1;
        while (i >= 0) {
            if (sortie[(size_t)i] == '.') { i--; continue; }
            sortie[(size_t)i]++;
            if (sortie[(size_t)i] <= '9') break;
            sortie[(size_t)i] = '0'; i--;
        }
    }
    free(brut);
    return sortie;
}
static void rose_pow_rationnel(mpq_class &dest, const mpq_class &base, long e) {
    mpz_class num = base.get_num(), den = base.get_den();
    if (e < 0) {
        if (mpz_sgn(num.get_mpz_t()) == 0) erreur_runtime("0^exposant négatif");
        std::swap(num, den);
        e = -e;
    }
    mpz_pow_ui(num.get_mpz_t(), num.get_mpz_t(), (unsigned long)e);
    mpz_pow_ui(den.get_mpz_t(), den.get_mpz_t(), (unsigned long)e);
    dest = mpq_class(num, den);
    dest.canonicalize();
}

/* ── interpréteur ─────────────────────────────────────────────────── */
struct RetourSignale { Valeur v; };

static int rnk(const Valeur &v) {
    switch (v.ty) {
        case T_BOOLEEN: case T_ENTIER: return 1;
        case T_RATIONNEL: return 2;
        case T_FLOTTANT: return 3;
    }
    return 0;
}

static Valeur convertir(Valeur v, int ty) {
    if (ty == T_ENTIER) {
        if (v.ty == T_BOOLEEN) return Valeur::de_z(v.b ? 1 : 0);
        if (v.ty != T_ENTIER) return Valeur::de_z(vers_z(v));
        return v;
    }
    if (ty == T_RATIONNEL) { if (v.ty != T_RATIONNEL) return Valeur::de_q(vers_q(v)); return v; }
    if (ty == T_FLOTTANT)  { if (v.ty != T_FLOTTANT) return Valeur::de_f(vers_d(v)); return v; }
    return v;
}

static int cmp_vals(const Valeur &g, const Valeur &d) {
    if (g.ty == T_CHAINE && d.ty == T_CHAINE) return g.s == d.s ? 0 : (g.s < d.s ? -1 : 1);
    bool flot = g.ty == T_FLOTTANT || d.ty == T_FLOTTANT;
    if (flot) {
        double A = vers_d(g), B = vers_d(d);
        return A < B ? -1 : (A > B ? 1 : 0);
    }
    if (g.ty == T_RATIONNEL || d.ty == T_RATIONNEL) {
        mpq_class A = vers_q(g), B = vers_q(d);
        return mpq_cmp(A.get_mpq_t(), B.get_mpq_t());
    }
    mpz_class A = vers_z(g), B = vers_z(d);
    return mpz_cmp(A.get_mpz_t(), B.get_mpz_t());
}

static bool valeur_egale(const Valeur &a, const Valeur &b);
static bool rose_valeur_egale(const Valeur &a, const Valeur &b) {
    if (a.ty == T_TAMPON || b.ty == T_TAMPON)
        return a.ty == T_TAMPON && b.ty == T_TAMPON && a.o && b.o && *a.o == *b.o;
    if (a.ty == T_LISTE || b.ty == T_LISTE) {
        if (a.ty != T_LISTE || b.ty != T_LISTE || (a.l ? a.l->size() : 0) != (b.l ? b.l->size() : 0)) return false;
        if (a.l) for (size_t i = 0; i < a.l->size(); i++)
            if (!rose_valeur_egale((*a.l)[i], (*b.l)[i])) return false;
        return true;
    }
    if (a.ty == T_TABLE || b.ty == T_TABLE) {
        if (a.ty != T_TABLE || b.ty != T_TABLE || (a.t ? a.t->size() : 0) != (b.t ? b.t->size() : 0)) return false;
        if (a.t) for (const auto &kv : *a.t) {
            bool found = false;
            if (b.t) for (const auto &kv2 : *b.t)
                if (rose_valeur_egale(kv.first, kv2.first) && rose_valeur_egale(kv.second, kv2.second)) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }
    if (a.ty == T_CHAINE || b.ty == T_CHAINE)
        return a.ty == T_CHAINE && b.ty == T_CHAINE && a.s == b.s;
    if (a.ty == T_BOOLEEN || b.ty == T_BOOLEEN) {
        if (a.ty == T_BOOLEEN && b.ty == T_BOOLEEN) return a.b == b.b;
        return false;
    }
    return cmp_vals(a, b) == 0;
}

/* ── aléatoire : splitmix64, reproductible via graine(n) ───────────────
   (générateur non cryptographique, semé par pid + adresse ASLR par défaut) */
static uint64_t g_rng_etat = (uint64_t)getpid() * 0x9E3779B97F4A7C15ULL
                           ^ (uint64_t)(uintptr_t)&g_rng_etat;
static uint64_t rng_tirer() {
    uint64_t z = (g_rng_etat += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* ══════════ temps() / duree() — chronomètre monotone et format adaptatif ══════════
   SPEC (identique dans les cinq moteurs, v3.13) : temps() renvoie les
   nanosecondes monotones écoulées depuis le démarrage du moteur. duree(n)
   met en forme n nanosecondes (n >= 0) : l'unité retenue est la plus grande
   de ns, µs, ms, s, min, h dont l'échelle (1, 10^3, 10^6, 10^9, 60.10^9,
   3600.10^9 ns) est <= n ; la fraction est écrite en jusqu'à 3 décimales
   TRONQUÉES (jamais arrondies), sans zéros superflus, séparateur « . ».
   La division se fait sur la chaîne décimale de n : un seul algorithme pour
   tous les moteurs, y compris sur les grands entiers GMP.
   ─────────────────────────────────────────────────────────────────────── */
static const std::chrono::steady_clock::time_point ROSE_DEPART =
        std::chrono::steady_clock::now();

// (chaîne décimale sans zéros de tête, reste) de n_str / div.
static std::pair<std::string, uint64_t> div_decimale_u64(const std::string &n_str, uint64_t div) {
    std::string q;
    uint64_t r = 0;
    for (char c : n_str) {
        uint64_t cour = r * 10 + (uint64_t)(c - '0');   // reste < div <= 3,6e12 → x10 sans dépassement
        q.push_back((char)('0' + cour / div));
        r = cour % div;
    }
    size_t z = q.find_first_not_of('0');
    return {z == std::string::npos ? std::string("0") : q.substr(z), r};
}

static std::string formater_duree(const std::string &n_str_in) {
    static const char *unit[] = {" ns", " µs", " ms", " s", " min", " h"};
    static const uint64_t scale[] = {1ULL, 1000ULL, 1000000ULL, 1000000000ULL,
                                     60000000000ULL, 3600000000000ULL};
    std::string n_str = n_str_in;
    size_t z = n_str.find_first_not_of('0');
    n_str = z == std::string::npos ? "0" : n_str.substr(z);
    auto au_moins = [&](uint64_t s) {                   // n >= s par ampleur décimale
        std::string t = std::to_string(s);
        return n_str.size() != t.size() ? n_str.size() > t.size() : n_str >= t;
    };
    int u = 0;
    for (int i = 1; i <= 5; i++) if (au_moins(scale[i])) u = i;
    auto [part, reste] = div_decimale_u64(n_str, scale[u]);
    if (u == 0 || reste == 0) return part + unit[u];
    uint64_t frac3 = (uint64_t)((reste * 1000ULL) / scale[u]);   // troncature, reste*1000 < 3,6e15
    std::string fs = std::to_string(frac3);
    while (fs.size() < 3) fs = "0" + fs;
    while (!fs.empty() && fs.back() == '0') fs.pop_back();
    return part + "." + fs + unit[u];
}

static Valeur stdlib(const std::string &nom, std::vector<Valeur> &a, int ligne) {
        if (nom == "temps") {
            long long ns = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - ROSE_DEPART).count();
            return Valeur::de_z(mpz_class(std::to_string(ns)));
        }
        if (nom == "duree") {
            if (mpz_sgn(a[0].z.get_mpz_t()) < 0)
                erreur_runtime("duree : la durée doit être positive ou nulle");
            return Valeur::de_s(formater_duree(a[0].z.get_str()));
        }
        if (nom == "ordre") {
            if (utf8_len(a[0].s) != 1)
                erreur_runtime("ordre attend une chaîne d'un seul caractère");
            return Valeur::de_z(mpz_class(utf8_char_at(a[0].s, 0)));
        }
        if (nom == "caractere") {
            if (!mpz_fits_ulong_p(a[0].z.get_mpz_t())
                    || mpz_cmp_ui(a[0].z.get_mpz_t(), 0x10FFFFUL) > 0)
                erreur_runtime("caractere : code hors plage (0 à 1114111)");
            unsigned long cp = mpz_get_ui(a[0].z.get_mpz_t());
            if (cp >= 0xD800 && cp <= 0xDFFF)
                erreur_runtime("caractere : code réservé (55296 à 57343)");
            return Valeur::de_s(utf8_encode(cp));
        }
        if (nom == "sous_chaine") {
            if (mpz_sgn(a[1].z.get_mpz_t()) < 0 || mpz_sgn(a[2].z.get_mpz_t()) < 0
                    || !mpz_fits_ulong_p(a[1].z.get_mpz_t()) || !mpz_fits_ulong_p(a[2].z.get_mpz_t()))
                erreur_runtime("sous_chaine : les bornes doivent être des entiers positifs");
            size_t taille = utf8_len(a[0].s);
            size_t d = (size_t)mpz_get_ui(a[1].z.get_mpz_t());
            size_t f = (size_t)mpz_get_ui(a[2].z.get_mpz_t());
            if (d > taille || f < d) erreur_runtime("sous_chaine : bornes invalides");
            if (f > taille) f = taille;
            size_t o1 = utf8_off(a[0].s, d);
            size_t o2 = (f == taille) ? a[0].s.size() : utf8_off(a[0].s, f);
            return Valeur::de_s(a[0].s.substr(o1, o2 - o1));
        }
        if (nom == "concatener") {
            if (!a[0].l) erreur_runtime("concatener attend une liste");
            std::string r;
            for (const Valeur &e : *a[0].l) {
                if (e.ty != T_CHAINE) erreur_runtime("concatener attend une liste de chaînes");
                r += e.s;
            }
            return Valeur::de_s(std::move(r));
        }
        if (nom == "lire_texte") {
            std::ifstream in(ROSE_CHEMIN(a[0].s), std::ios::binary);
            if (!in) erreur_runtime("lecture du fichier impossible (" + a[0].s + ")");
            std::stringstream ss; ss << in.rdbuf();
            if (ss.str().size() > ((size_t)1 << 28))
                erreur_runtime("fichier trop volumineux (" + a[0].s + ")");
            return Valeur::de_s(ss.str());
        }
        if (nom == "ecrire_texte") {
            std::ofstream out(ROSE_CHEMIN(a[0].s), std::ios::binary | std::ios::trunc);
            if (!out) erreur_runtime("écriture du fichier impossible (" + a[0].s + ")");
            out << a[1].s;
            out.flush();
            if (!out) erreur_runtime("écriture du fichier impossible (" + a[0].s + ")");
            return Valeur::vide();
        }
        if (nom == "nb_arguments")
            return Valeur::de_z(mpz_class((unsigned long)ARGUMENTS_PROG.size()));
        if (nom == "argument") {
            if (!mpz_fits_ulong_p(a[0].z.get_mpz_t())
                    || mpz_cmp_ui(a[0].z.get_mpz_t(), ARGUMENTS_PROG.size()) >= 0)
                erreur_runtime("index hors des limites des arguments");
            return Valeur::de_s(ARGUMENTS_PROG[(size_t)mpz_get_ui(a[0].z.get_mpz_t())]);
        }
        if (nom == "afficher_erreur") { ecri(stderr, a[0]); fputc('\n', stderr); fflush(stderr); return Valeur::vide(); }
        if (nom == "quitter") {
            if (!mpz_fits_sint_p(a[0].z.get_mpz_t()))
                erreur_runtime("code de sortie hors plage");
            std::fflush(stdout); std::fflush(stderr);
            throw Quitter{ (int)mpz_get_si(a[0].z.get_mpz_t()) };
        }
        if (nom == "afficher") { afficher(a[0]); return Valeur::vide(); }
        if (nom == "saisir_entier") return lire_selon(T_ENTIER);
        if (nom == "saisir_flottant") return lire_selon(T_FLOTTANT);
        if (nom == "millisecondes") {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return Valeur::de_z(mpz_class((unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000)));
        }
        if (nom == "graine") {
            mpz_class s = vers_z(a[0]);
            g_rng_etat = mpz_fits_ulong_p(s.get_mpz_t()) ? mpz_get_ui(s.get_mpz_t())
                : (uint64_t)mpz_fdiv_ui(s.get_mpz_t(), 0xFFFFFFFFu) * 0x100000000ULL
                + mpz_fdiv_ui(s.get_mpz_t(), 0x100000000u);
            rng_tirer();   // casse la corrélation directe avec la graine
            return Valeur::vide();
        }
        if (nom == "hasard") {
            mpz_class n = vers_z(a[0]);
            if (mpz_sgn(n.get_mpz_t()) <= 0) erreur_runtime("hasard : bornage positif attendu");
            if (mpz_fits_ulong_p(n.get_mpz_t())) {
                unsigned long nn = mpz_get_ui(n.get_mpz_t());
                uint64_t r;
                if ((nn & (nn - 1)) == 0) r = rng_tirer() & (uint64_t)(nn - 1);
                else r = (uint64_t)((__uint128_t)rng_tirer() * nn >> 64);   // réduction de Lemire
                return Valeur::de_z(mpz_class((unsigned long)r));
            }
            uint64_t r64 = rng_tirer();
            mpz_class rr;
            mpz_set_ui(rr.get_mpz_t(), (unsigned long)(r64 >> 32));
            mpz_mul_2exp(rr.get_mpz_t(), rr.get_mpz_t(), 32);
            mpz_add_ui(rr.get_mpz_t(), rr.get_mpz_t(), (unsigned long)(r64 & 0xFFFFFFFFu));
            mpz_mod(rr.get_mpz_t(), rr.get_mpz_t(), n.get_mpz_t());
            return Valeur::de_z(rr);
        }
        if (nom == "tampon") {
            mpz_class n = vers_z(a[0]);
            if (mpz_sgn(n.get_mpz_t()) < 0 || !mpz_fits_ulong_p(n.get_mpz_t()))
                erreur_runtime("tampon : taille démesurée");
            return Valeur::de_o(std::make_shared<std::string>(mpz_get_ui(n.get_mpz_t()), '\0'));
        }
        if (nom == "octets") return Valeur::de_o(std::make_shared<std::string>(a[0].s));
        if (nom == "tampon_case") {
            if (a[0].ty != T_TAMPON || !a[0].o) erreur_runtime("tampon_case : tampon attendu");
            if (!mpz_fits_slong_p(vers_z(a[1]).get_mpz_t())
                    || mpz_get_si(vers_z(a[1]).get_mpz_t()) < 0
                    || (size_t)mpz_get_si(vers_z(a[1]).get_mpz_t()) >= a[0].o->size())
                erreur_runtime("index hors tampon");
            return Valeur::de_z(mpz_class(
                (unsigned long)(unsigned char)(*a[0].o)[(size_t)mpz_get_si(vers_z(a[1]).get_mpz_t())]));
        }
        if (nom == "tampon_pose") {
            if (a[0].ty != T_TAMPON || !a[0].o) erreur_runtime("tampon_pose : tampon attendu");
            if (!mpz_fits_slong_p(vers_z(a[1]).get_mpz_t())
                    || mpz_get_si(vers_z(a[1]).get_mpz_t()) < 0
                    || (size_t)mpz_get_si(vers_z(a[1]).get_mpz_t()) >= a[0].o->size())
                erreur_runtime("index hors tampon");
            (*a[0].o)[(size_t)mpz_get_si(vers_z(a[1]).get_mpz_t())] =
                (char)mpz_fdiv_ui(vers_z(a[2]).get_mpz_t(), 256);
            return Valeur::vide();
        }
        if (nom == "remplir_hasard") {
            if (a[0].ty != T_TAMPON || !a[0].o) erreur_runtime("remplir_hasard : tampon attendu");
            if (a[1].ty != T_TAMPON || !a[1].o || a[1].o->empty())
                erreur_runtime("remplir_hasard : alphabet non vide attendu");
            std::string &buf = *a[0].o;
            const char *ab = a[1].o->data();
            const size_t s = a[1].o->size();
            if (s == 1) {                       // alphabet mono-caractère
                buf.assign(buf.size(), ab[0]);
            } else if ((s & (s - 1)) == 0 && s <= 256) {
                // Puissance de deux ≤ 256 : un octet par caractère, masque
                // exact, 8 caractères par tirage — et quatre tirages
                // indépendants par lot pour cacher la latence de splitmix.
                const size_t m = s - 1;
                unsigned char *p = (unsigned char *)&buf[0];
                const size_t n = buf.size();
                size_t i = 0;
                for (; i + 32 <= n; i += 32) {
                    uint64_t r0 = rng_tirer(), r1 = rng_tirer();
                    uint64_t r2 = rng_tirer(), r3 = rng_tirer();
                    for (int k = 0; k < 8; k++) {
                        p[i + k]      = (unsigned char)ab[(r0 >> (8 * k)) & m];
                        p[i + 8 + k]  = (unsigned char)ab[(r1 >> (8 * k)) & m];
                        p[i + 16 + k] = (unsigned char)ab[(r2 >> (8 * k)) & m];
                        p[i + 24 + k] = (unsigned char)ab[(r3 >> (8 * k)) & m];
                    }
                }
                for (; i < n; i++) p[i] = (unsigned char)ab[rng_tirer() & m];
            } else if ((s & (s - 1)) == 0) {   // puissance de deux > 256
                const size_t mask = s - 1;
                const int bits = 64 - __builtin_clzll((unsigned long long)s);
                const size_t per = (size_t)(64 / bits);
                size_t i = 0;
                for (; i + per <= buf.size(); i += per) {
                    uint64_t r = rng_tirer();
                    for (size_t k = 0; k < per; k++)
                        buf[i + k] = ab[(r >> (k * (size_t)bits)) & mask];
                }
                for (; i < buf.size(); i++) buf[i] = ab[rng_tirer() & mask];
            } else {                            // sinon Lemire, uniforme sans biais
                for (size_t i = 0; i < buf.size(); i++)
                    buf[i] = ab[(size_t)((__uint128_t)rng_tirer() * s >> 64)];
            }
            return Valeur::vide();
        }
        if (nom == "factorielle") return Valeur::de_z(rose_factorielle(vers_z(a[0])));
        if (nom == "fibonacci") return Valeur::de_z(rose_fibonacci(vers_z(a[0])));
        if (nom == "pgcd") { mpz_class r; mpz_gcd(r.get_mpz_t(), vers_z(a[0]).get_mpz_t(), vers_z(a[1]).get_mpz_t()); return Valeur::de_z(r); }
        if (nom == "ppcm") { mpz_class r; mpz_lcm(r.get_mpz_t(), vers_z(a[0]).get_mpz_t(), vers_z(a[1]).get_mpz_t()); return Valeur::de_z(r); }
        if (nom == "puissance") {
            long e = 0;
            mpz_class expo = vers_z(a[1]);
            if (mpz_fits_slong_p(expo.get_mpz_t())) e = mpz_get_si(expo.get_mpz_t());
            else erreur_runtime("exposant démesuré");
            if (a[0].ty == T_FLOTTANT) return Valeur::de_f(std::pow(a[0].f, (double)e));
            if (a[0].ty == T_RATIONNEL) { mpq_class r; rose_pow_rationnel(r, a[0].q, e); return Valeur::de_q(r); }
            if (e < 0) {
                if (mpz_sgn(a[0].z.get_mpz_t()) == 0) erreur_runtime("0^exposant négatif");
                mpq_class r; rose_pow_rationnel(r, mpq_class(a[0].z), e); return Valeur::de_q(r);
            }
            if (e < 0) erreur_runtime("exposant entier négatif : le résultat est rationnel");
            mpz_class r;
            mpz_pow_ui(r.get_mpz_t(), vers_z(a[0]).get_mpz_t(), (unsigned long)e);
            return Valeur::de_z(r);
        }
        if (nom == "est_premier") return Valeur::de_b(rose_est_premier(vers_z(a[0])));
        if (nom == "compter_premiers") return Valeur::de_z(rose_compter_premiers(vers_z(a[0])));
        if (nom == "nb_chiffres") return Valeur::de_z(rose_nb_chiffres(vers_z(a[0])));
        if (nom == "nombre_pi") {
            return Valeur::de_s(rose_pi(vers_z(a[0])));
        }
        if (nom == "longueur") {
            if (a[0].ty == T_TAMPON)
                return Valeur::de_z(mpz_class(a[0].o ? (unsigned long)a[0].o->size() : 0UL));
            if (a[0].ty == T_CHAINE)
                return Valeur::de_z(mpz_class((unsigned long)utf8_len(a[0].s)));
            if (a[0].ty == T_TABLE)
                return Valeur::de_z(mpz_class(a[0].t ? (unsigned long)a[0].t->size() : 0UL));
            return Valeur::de_z(mpz_class(a[0].l ? (unsigned long)a[0].l->size() : 0UL));
        }
        if (nom == "table") {
            auto t = std::make_shared<std::vector<std::pair<Valeur,Valeur>>>();
            return Valeur::de_t(t);
        }
        if (nom == "inserer") {
            if (a[0].ty != T_TABLE || !a[0].t) erreur_runtime("inserer attend une table");
            auto &vec = *a[0].t;
            for (auto &kv : vec)
                if (rose_valeur_egale(kv.first, a[1])) { kv.second = a[2]; return Valeur::vide(); }
            vec.push_back({a[1], a[2]});
            return Valeur::vide();
        }
        if (nom == "cle_existe") {
            if (a[0].ty != T_TABLE || !a[0].t) erreur_runtime("cle_existe attend une table");
            for (const auto &kv : *a[0].t)
                if (rose_valeur_egale(kv.first, a[1])) return Valeur::de_b(true);
            return Valeur::de_b(false);
        }
        if (nom == "supprimer") {
            if (a[0].ty != T_TABLE || !a[0].t) erreur_runtime("supprimer attend une table");
            auto &vec = *a[0].t;
            for (size_t i = 0; i < vec.size(); i++)
                if (rose_valeur_egale(vec[i].first, a[1])) { vec.erase(vec.begin() + i); return Valeur::de_b(true); }
            return Valeur::de_b(false);
        }
        if (nom == "cles") {
            if (a[0].ty != T_TABLE || !a[0].t) erreur_runtime("cles attend une table");
            auto l = std::make_shared<std::vector<Valeur>>();
            for (const auto &kv : *a[0].t) l->push_back(kv.first);
            return Valeur::de_l(l);
        }
        if (nom == "valeurs") {
            if (a[0].ty != T_TABLE || !a[0].t) erreur_runtime("valeurs attend une table");
            auto l = std::make_shared<std::vector<Valeur>>();
            for (const auto &kv : *a[0].t) l->push_back(kv.second);
            return Valeur::de_l(l);
        }
        if (nom == "joindre") {
            if (!a[0].l) erreur_runtime("joindre attend une liste");
            auto &vec = *a[0].l;
            if (!vec.empty()) a[1] = convertir(std::move(a[1]), vec.front().ty);
            vec.push_back(a[1]);
            return Valeur::vide();
        }
        if (nom == "abs") {
            if (a[0].ty == T_ENTIER || a[0].ty == T_BOOLEEN) { mpz_class z = vers_z(a[0]); mpz_abs(z.get_mpz_t(), z.get_mpz_t()); return Valeur::de_z(z); }
            if (a[0].ty == T_RATIONNEL) { mpq_class q = a[0].q; mpz_abs(q.get_num().get_mpz_t(), q.get_num().get_mpz_t()); q.canonicalize(); return Valeur::de_q(q); }
            return Valeur::de_f(std::fabs(a[0].f));
        }
        if (nom == "min" || nom == "max") {
            int m = std::max(rnk(a[0]), rnk(a[1]));
            Valeur x = convertir(a[0], m == 3 ? T_FLOTTANT : m == 2 ? T_RATIONNEL : T_ENTIER);
            Valeur y = convertir(a[1], m == 3 ? T_FLOTTANT : m == 2 ? T_RATIONNEL : T_ENTIER);
            int c = cmp_vals(x, y);
            return (nom == "min") ? (c <= 0 ? x : y) : (c >= 0 ? x : y);
        }
        if (nom == "partie_entiere") {
            if (a[0].ty == T_ENTIER || a[0].ty == T_BOOLEEN) return Valeur::de_z(vers_z(a[0]));
            if (a[0].ty == T_RATIONNEL) {
                mpz_class r;
                mpz_fdiv_q(r.get_mpz_t(), a[0].q.get_num().get_mpz_t(), a[0].q.get_den().get_mpz_t());
                return Valeur::de_z(r);
            }
            mpz_class r; mpz_set_d(r.get_mpz_t(), std::floor(a[0].f));
            return Valeur::de_z(r);
        }
        if (nom == "fraction") {
            if (mpz_sgn(vers_z(a[1]).get_mpz_t()) == 0) erreur_runtime("fraction de dénominateur nul");
            return Valeur::de_q(mpq_class(vers_z(a[0]), vers_z(a[1])));
        }
        erreur_runtime("fonction '" + nom + "' inconnue [Ligne " + std::to_string(ligne) + "]");
        return Valeur::vide();
    }

/* ═══════════════════════════════════════════════════════════════════
   RIR — Représentation Intermédiaire de Rose (jalon M1)

   Le vérificateur émet du RIR ; tous les backends (interpréteur RIR,
   JIT, LLVM) consomment du RIR. Une seule sémantique.
   ═══════════════════════════════════════════════════════════════════ */

/* ── types RIR ─────────────────────────────────────────────────────── */
enum class RirType { Z, Q, F, B, S, U, Tab, L = 7, TBL = 9 };
/* v3.14.1 — L et TBL s'alignent sur les valeurs 7/9 du RirListe/RirTbl du
   transpileur (continuité numérique des deux enums) : les paramètres de
   fonction portent désormais leur type agrégé, là où ty_vers_rir aplatis-
   sait tout en U — ce qui faisait émettre au AOT un « rose_val » inexistant
   en signature. Le transpileur garde Gd(8)/Dyn(10) internes. */

/* ── opérande RIR ─────────────────────────────────────────────────── */
struct RirOp {
    enum Kind { Const, Temp, Global, Label } kind;
    std::string name;        /* nom du temporaire/global/label */
    int const_idx = -1;      /* index dans la table des constantes */
    RirType ty = RirType::U;
};

/* ── instruction RIR ───────────────────────────────────────────────── */
struct RirInstr {
    std::string dest;        /* temporaire de destination (%tN) ou vide */
    RirType dest_ty = RirType::U;
    std::string op;          /* nom de l'opération (add.z, cmp.lt, br.cond, ret.z, call, etc.) */
    std::vector<RirOp> args;
    std::string label_cible; /* pour br/br.cond */
    std::string label_cible2;
    std::string message;     /* pour erreur "msg" */
};

/* ── bloc de base RIR ─────────────────────────────────────────────── */
struct RirBloc {
    std::string nom;         /* .entree, .alors, .sinon, ... */
    std::vector<RirInstr> instrs;
};

/* ── fonction RIR ─────────────────────────────────────────────────── */
struct RirFonction {
    std::string nom;         /* $fact, $principal */
    std::vector<std::pair<std::string, RirType>> params;  /* (nom, type) */
    RirType retour = RirType::U;
    std::vector<RirBloc> blocs;
};

/* ── module RIR ────────────────────────────────────────────────────── */
struct RirModule {
    int version = 1;
    /* constantes nommées */
    struct Const { std::string nom; RirType ty; std::string texte; };
    std::vector<Const> consts;
    std::vector<RirFonction> fonctions;
};

/* ── convertisseur Ty → RirType ────────────────────────────────────── */
static RirType ty_vers_rir(Ty t) {
    switch (t.n) {
        case T_ENTIER: return RirType::Z;
        case T_RATIONNEL: return RirType::Q;
        case T_FLOTTANT: return RirType::F;
        case T_BOOLEEN: return RirType::B;
        case T_CHAINE: return RirType::S;
        case T_LISTE: return RirType::L;   /* v3.14.1 : le paramètre porte son type (avant : U → rose_val AOT) */
        case T_TABLE: return RirType::TBL; /* idem ; l'appel passe-table reste un rejet AOT poli (M4.2) */
        case T_VALEUR: return RirType::U;
        default: return RirType::U;
    }
}

static std::string nom_rir_type(RirType t) {
    switch (t) {
        case RirType::Z: return "z";
        case RirType::Q: return "q";
        case RirType::F: return "f";
        case RirType::B: return "b";
        case RirType::S: return "s";
        case RirType::U: return "u";
        case RirType::Tab: return "tab";
        case RirType::L: return "l";        /* v3.14.1 */
        case RirType::TBL: return "tbl";    /* v3.14.1 */
    }
    return "?";
}

/* ── émetteur RIR : AST vérifié → RIR ───────────────────────────── */
struct EmetteurRir {
    RirModule &mod;
    EmetteurRir(RirModule &m) : mod(m) {}
    int compteur_temp = 0;
    std::string nouveau_temp() { return "%t" + std::to_string(compteur_temp++); }
    int compteur_bloc = 0;
    /* Nom de bloc unique : le numéro de ligne seul collide quand deux
       structures figurent sur la même ligne physique (`si` imbriqués écrits
       sur une ligne), ce qui mélange les cibles de branchement. */
    std::string suffixe_bloc() { return "_" + std::to_string(compteur_bloc++); }

    /* émet une expression, retourne le temporaire contenant le résultat */
    std::string emettre_expr(PE e, RirBloc &bloc, RirType ty_attendu) {
        if (ty_attendu == RirType::U && e->type != T_VIDE)
            ty_attendu = ty_vers_rir(Ty(e->type));
        /* comparaison entre tampons : égalité générique (les opérateurs
           numériques forceraient une conversion vers entier) */
        if (e->k == NK::Binaire && (e->texte == "==" || e->texte == "!=")) {
            Ty tg(e->a ? e->a->type : T_VIDE), td(e->b ? e->b->type : T_VIDE);
            if (tg.n == T_TAMPON || td.n == T_TAMPON) {
                std::string g = emettre_expr(e->a, bloc, RirType::U);
                std::string d = emettre_expr(e->b, bloc, RirType::U);
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t; i.dest_ty = RirType::B;
                i.op = e->texte == "==" ? "cmp.eq.b" : "cmp.ne.b";
                RirOp o1; o1.kind = RirOp::Temp; o1.name = g; i.args.push_back(o1);
                RirOp o2; o2.kind = RirOp::Temp; o2.name = d; i.args.push_back(o2);
                bloc.instrs.push_back(i);
                return t;
            }
        }
        switch (e->k) {
            case NK::Litteral: {
                /* constante nommée */
                std::string nom_const = "@c" + e->texte;
                /* simplification : on émet la constante inline */
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t; i.dest_ty = ty_attendu; i.op = "cst." + nom_rir_type(ty_attendu);
                RirOp o; o.kind = RirOp::Const; o.name = nom_const; o.ty = ty_attendu;
                i.args.push_back(o);
                bloc.instrs.push_back(i);
                return t;
            }
            case NK::ChaineLit: {
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t; i.dest_ty = RirType::S; i.op = "cst.s";
                RirOp o; o.kind = RirOp::Const; o.name = e->texte; o.ty = RirType::S;
                i.args.push_back(o);
                bloc.instrs.push_back(i);
                return t;
            }
            case NK::BooleenLit: {
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t; i.dest_ty = RirType::B; i.op = "cst.b";
                RirOp o; o.kind = RirOp::Const; o.name = e->flag ? "vrai" : "faux"; o.ty = RirType::B;
                i.args.push_back(o);
                bloc.instrs.push_back(i);
                return t;
            }
            case NK::Var: {
                RirOp o; o.kind = RirOp::Temp; o.name = "%" + e->texte; o.ty = ty_attendu;
                return o.name;
            }
            case NK::Index: {
                std::string base = emettre_expr(e->a, bloc, RirType::U);
                std::string idx = emettre_expr(e->b, bloc, RirType::Z);
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t;
                /* v3.14.1 — le type du résultat d'index est celui que le
                   checker a calculé (élément de liste<chaine> → S, de
                   liste<entier> → Z, cellule de table → table, caractère de
                   chaîne → S). L'écraser à U faisait dériver le AOT vers des
                   declarations fausses (rose_val fantôme ou rose_ent pour une
                   chaine). */
                i.dest_ty = e->type != T_VIDE ? ty_vers_rir(Ty(e->type)) : RirType::U;
                i.op = "index";
                RirOp o1; o1.kind = RirOp::Temp; o1.name = base; i.args.push_back(o1);
                RirOp o2; o2.kind = RirOp::Temp; o2.name = idx; i.args.push_back(o2);
                bloc.instrs.push_back(i);
                return t;
            }
            case NK::Binaire: {
                std::string g = emettre_expr(e->a, bloc, RirType::U);
                std::string d = emettre_expr(e->b, bloc, RirType::U);
                std::string t = nouveau_temp();
                std::string op_nom = e->texte;
                /* mapper les opérateurs Rose → RIR */
                if (op_nom == "+") op_nom = "add";
                else if (op_nom == "-") op_nom = "sub";
                else if (op_nom == "*") op_nom = "mul";
                else if (op_nom == "/") op_nom = "div";     /* exacte → q */
                else if (op_nom == "div") op_nom = "divf";  /* euclidienne (spec IR.md) */
                else if (op_nom == "^") op_nom = "pow";
                else if (op_nom == "%") op_nom = "rem.z";
                else if (op_nom == "==") op_nom = "cmp.eq";
                else if (op_nom == "!=") op_nom = "cmp.ne";
                else if (op_nom == "<") op_nom = "cmp.lt";
                else if (op_nom == "<=") op_nom = "cmp.le";
                else if (op_nom == ">") op_nom = "cmp.gt";
                else if (op_nom == ">=") op_nom = "cmp.ge";
                else if (op_nom == "et") op_nom = "and";
                else if (op_nom == "ou") op_nom = "or";
                RirInstr i; i.dest = t; i.dest_ty = ty_attendu;
                /* comparaison tolérante : nom propre, indépendante du type */
                if (op_nom == "≈" || op_nom == "?=" || op_nom == "=~") op_nom = "cmp.approx";
                if (op_nom == "cmp.approx") {
                    i.op = op_nom;
                } else {
                    i.op = op_nom + "." + nom_rir_type(ty_attendu);
                }
                RirOp o1; o1.kind = RirOp::Temp; o1.name = g; i.args.push_back(o1);
                RirOp o2; o2.kind = RirOp::Temp; o2.name = d; i.args.push_back(o2);
                bloc.instrs.push_back(i);
                return t;
            }
            case NK::Unaire: {
                std::string a = emettre_expr(e->a, bloc, ty_attendu);
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t; i.dest_ty = ty_attendu;
                if (e->texte == "-") i.op = "neg." + nom_rir_type(ty_attendu);
                else if (e->texte == "non") i.op = "not.b";
                else i.op = "unary." + e->texte;
                RirOp o; o.kind = RirOp::Temp; o.name = a; i.args.push_back(o);
                bloc.instrs.push_back(i);
                return t;
            }
            case NK::Appel: {
                std::vector<std::string> arg_temps;
                for (PE &a : e->args) {
                    arg_temps.push_back(emettre_expr(a, bloc, RirType::U));
                }
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t; i.dest_ty = ty_attendu; i.op = "call";
                RirOp o; o.kind = RirOp::Temp; o.name = "$" + e->texte; o.ty = ty_attendu;
                i.args.push_back(o);
                for (auto &at : arg_temps) {
                    RirOp oa; oa.kind = RirOp::Temp; oa.name = at; i.args.push_back(oa);
                }
                bloc.instrs.push_back(i);
                return t;
            }
            case NK::Factorielle: {
                std::string a = emettre_expr(e->a, bloc, RirType::Z);
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t; i.dest_ty = RirType::Z; i.op = "call @builtin.factorielle";
                RirOp o; o.kind = RirOp::Temp; o.name = a; i.args.push_back(o);
                bloc.instrs.push_back(i);
                return t;
            }
            case NK::ListeLit: {
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t; i.dest_ty = RirType::U; i.op = "liste.new";
                bloc.instrs.push_back(i);
                for (PE &a : e->args) {
                    std::string v = emettre_expr(a, bloc, RirType::U);
                    RirInstr p; p.op = "liste.push"; p.args.push_back(RirOp{RirOp::Temp, t, -1, RirType::U});
                    RirOp vo; vo.kind = RirOp::Temp; vo.name = v; p.args.push_back(vo);
                    bloc.instrs.push_back(p);
                }
                return t;
            }
            default:
                return "%?";
        }
    }

    /* émet une instruction Rose en RIR */
    void emettre_instr(PI instr, RirFonction &fn, RirBloc &bloc) {
        switch (instr->k) {
            case IK::Afficher: {
                std::string v = emettre_expr(instr->expr, bloc, RirType::U);
                RirInstr i; i.op = "afficher"; 
                RirOp o; o.kind = RirOp::Temp; o.name = v; i.args.push_back(o);
                bloc.instrs.push_back(i);
                break;
            }
            case IK::Lire: {
                std::string t = nouveau_temp();
                RirInstr i; i.dest = t; i.dest_ty = RirType::U; i.op = "lire";
                i.message = std::to_string(instr->type_read);   /* type cible de la lecture */
                RirOp o; o.kind = RirOp::Temp; o.name = "%" + instr->nom; i.args.push_back(o);
                bloc.instrs.push_back(i);
                RirInstr a; a.op = "affect"; a.message = instr->nom;
                RirOp oa; oa.kind = RirOp::Temp; oa.name = t; a.args.push_back(oa);
                bloc.instrs.push_back(a);
                break;
            }
            case IK::Affectation: {
                std::string v = emettre_expr(instr->expr, bloc, RirType::U);
                RirInstr i; i.op = "affect"; i.message = instr->nom;
                RirOp o; o.kind = RirOp::Temp; o.name = v; i.args.push_back(o);
                bloc.instrs.push_back(i);
                break;
            }
            case IK::Si: {
                std::string cond = emettre_expr(instr->test, bloc, RirType::B);
                std::string suf = suffixe_bloc();
                std::string alors_nom = ".alors_" + std::to_string(instr->ligne) + suf;
                std::string sinon_nom = ".sinon_" + std::to_string(instr->ligne) + suf;
                std::string fin_nom = ".fin_" + std::to_string(instr->ligne) + suf;
                RirInstr br; br.op = "br.cond"; 
                RirOp co; co.kind = RirOp::Temp; co.name = cond; br.args.push_back(co);
                br.label_cible = alors_nom; br.label_cible2 = sinon_nom;
                bloc.instrs.push_back(br);
                /* bloc alors — émis via le curseur fn.blocs.back() : un « si »
                   interne coupe le bloc et la suite du corps reprend dans son
                   bloc « fin » ; avec une cible fixe, elle resterait collée
                   après le br.cond (mort) et le « fin » interne choirait sur
                   un bloc voisin. */
                RirBloc ba; ba.nom = alors_nom;
                fn.blocs.push_back(ba);
                for (PI &i : instr->alors->instrs) emettre_instr(i, fn, fn.blocs.back());
                RirInstr br2; br2.op = "br"; br2.label_cible = fin_nom;
                fn.blocs.back().instrs.push_back(br2);
                /* bloc sinon */
                RirBloc bs; bs.nom = sinon_nom;
                fn.blocs.push_back(bs);
                if (instr->sinon) for (PI &i : instr->sinon->instrs) emettre_instr(i, fn, fn.blocs.back());
                RirInstr br3; br3.op = "br"; br3.label_cible = fin_nom;
                fn.blocs.back().instrs.push_back(br3);
                /* bloc fin */
                RirBloc bf; bf.nom = fin_nom;
                fn.blocs.push_back(bf);
                break;
            }
            case IK::TantQue: {
                std::string suf = suffixe_bloc();
                std::string cond_nom = ".cond_" + std::to_string(instr->ligne) + suf;
                std::string corps_nom = ".corps_" + std::to_string(instr->ligne) + suf;
                std::string fin_nom = ".fin_" + std::to_string(instr->ligne) + suf;
                RirInstr br; br.op = "br"; br.label_cible = cond_nom;
                bloc.instrs.push_back(br);
                RirBloc bc; bc.nom = cond_nom;
                std::string cond = emettre_expr(instr->test, bc, RirType::B);
                RirInstr brc; brc.op = "br.cond";
                RirOp co; co.kind = RirOp::Temp; co.name = cond; brc.args.push_back(co);
                brc.label_cible = corps_nom; brc.label_cible2 = fin_nom;
                bc.instrs.push_back(brc);
                fn.blocs.push_back(bc);
                RirBloc bcorps; bcorps.nom = corps_nom;
                fn.blocs.push_back(bcorps);
                /* le corps passe par le curseur fn.blocs.back() : un « si »
                   interne coupe le bloc et sa suite y reprendra le fil */
                for (PI &i : instr->corps->instrs) emettre_instr(i, fn, fn.blocs.back());
                RirInstr br2; br2.op = "br"; br2.label_cible = cond_nom;
                fn.blocs.back().instrs.push_back(br2);
                RirBloc bfin; bfin.nom = fin_nom;
                fn.blocs.push_back(bfin);
                break;
            }
            case IK::Pour: {
                /* pour $i de debut vers fin { corps } */
                std::string var = instr->nom;
                std::string suf = suffixe_bloc();
                std::string init_nom = ".init_" + std::to_string(instr->ligne) + suf;
                std::string cond_nom = ".cond_" + std::to_string(instr->ligne) + suf;
                std::string corps_nom = ".corps_" + std::to_string(instr->ligne) + suf;
                std::string incr_nom = ".incr_" + std::to_string(instr->ligne) + suf;
                std::string fin_nom = ".fin_" + std::to_string(instr->ligne) + suf;
                /* init */
                RirInstr ia; ia.op = "affect"; ia.message = var;
                std::string debut_t = emettre_expr(instr->debut, bloc, RirType::Z);
                RirOp oa; oa.kind = RirOp::Temp; oa.name = debut_t; ia.args.push_back(oa);
                bloc.instrs.push_back(ia);
                RirInstr br0; br0.op = "br"; br0.label_cible = cond_nom;
                bloc.instrs.push_back(br0);
                /* cond */
                RirBloc bcond; bcond.nom = cond_nom;
                std::string var_t = "%" + var;
                RirInstr cmp; cmp.op = "cmp.lt.z";
                RirOp v1; v1.kind = RirOp::Temp; v1.name = var_t; cmp.args.push_back(v1);
                std::string fin_t = emettre_expr(instr->fin, bcond, RirType::Z);
                RirOp v2; v2.kind = RirOp::Temp; v2.name = fin_t; cmp.args.push_back(v2);
                std::string cond_t = nouveau_temp(); cmp.dest = cond_t; cmp.dest_ty = RirType::B;
                bcond.instrs.push_back(cmp);
                RirInstr brc; brc.op = "br.cond";
                RirOp co; co.kind = RirOp::Temp; co.name = cond_t; brc.args.push_back(co);
                brc.label_cible = corps_nom; brc.label_cible2 = fin_nom;
                bcond.instrs.push_back(brc);
                fn.blocs.push_back(bcond);
                /* corps */
                RirBloc bcorps; bcorps.nom = corps_nom;
                fn.blocs.push_back(bcorps);
                /* corps via curseur : un « si » interne coupe et rebranche */
                for (PI &i : instr->corps->instrs) emettre_instr(i, fn, fn.blocs.back());
                RirInstr brincr; brincr.op = "br"; brincr.label_cible = incr_nom;
                fn.blocs.back().instrs.push_back(brincr);
                /* incr */
                RirBloc bincr; bincr.nom = incr_nom;
                RirInstr add; add.op = "add.z";
                RirOp va; va.kind = RirOp::Temp; va.name = var_t; add.args.push_back(va);
                RirOp one; one.kind = RirOp::Const; one.name = "@c1"; one.ty = RirType::Z; add.args.push_back(one);
                std::string newvar = nouveau_temp(); add.dest = newvar; add.dest_ty = RirType::Z;
                bincr.instrs.push_back(add);
                RirInstr aff; aff.op = "affect"; aff.message = var;
                RirOp nv; nv.kind = RirOp::Temp; nv.name = newvar; aff.args.push_back(nv);
                bincr.instrs.push_back(aff);
                RirInstr brcond; brcond.op = "br"; brcond.label_cible = cond_nom;
                bincr.instrs.push_back(brcond);
                fn.blocs.push_back(bincr);
                /* fin */
                RirBloc bfin; bfin.nom = fin_nom;
                fn.blocs.push_back(bfin);
                break;
            }
            case IK::Retour: {
                if (instr->expr) {
                    std::string v = emettre_expr(instr->expr, bloc, RirType::U);
                    RirInstr i; i.op = "ret"; 
                    RirOp o; o.kind = RirOp::Temp; o.name = v; i.args.push_back(o);
                    bloc.instrs.push_back(i);
                } else {
                    RirInstr i; i.op = "ret.u"; bloc.instrs.push_back(i);
                }
                break;
            }
            case IK::Expr: {
                emettre_expr(instr->expr, bloc, RirType::U);
                break;
            }
            case IK::Bloc: {
                for (PI &i : instr->instrs) emettre_instr(i, fn, bloc);
                break;
            }
            default: break;
        }
    }

    /* ── émission d'une fonction ───────────────────────────────────── */
    void emettre_fonction(Signature &sig) {
        RirFonction fn;
        fn.nom = "$" + sig.nom;
        for (auto &p : sig.params) {
            fn.params.push_back({p.first, ty_vers_rir(p.second)});
        }
        fn.retour = ty_vers_rir(sig.retour);
        RirBloc entree; entree.nom = ".entree";
        fn.blocs.push_back(entree);
        for (PI &i : sig.corps->instrs) emettre_instr(i, fn, fn.blocs.back());
        /* ret implicite */
        if (fn.blocs.back().instrs.empty() || fn.blocs.back().instrs.back().op.rfind("ret", 0) != 0) {
            RirInstr r; r.op = "ret.u"; fn.blocs.back().instrs.push_back(r);
        }
        mod.fonctions.push_back(fn);
    }

    /* ── émission du module principal ──────────────────────────────── */
    void emettre_principal(Programme &prog) {
        RirFonction fn;
        fn.nom = "$principal";
        fn.retour = RirType::U;
        RirBloc entree; entree.nom = ".entree";
        fn.blocs.push_back(entree);
        for (PI &i : prog.instructions) emettre_instr(i, fn, fn.blocs.back());
        if (fn.blocs.back().instrs.empty() || fn.blocs.back().instrs.back().op.rfind("ret", 0) != 0) {
            RirInstr r; r.op = "ret.u"; fn.blocs.back().instrs.push_back(r);
        }
        mod.fonctions.push_back(fn);
    }

    /* ── dump texte ────────────────────────────────────────────────── */
    std::string dump() {
        std::ostringstream out;
        out << "rir 1\n";
        /* constantes */
        for (auto &c : mod.consts) {
            out << "const " << c.nom << " @" << c.texte << " = " << c.texte << "\n";
        }
        /* fonctions */
        for (auto &f : mod.fonctions) {
            out << "\nfonction " << f.nom << "(";
            for (size_t i = 0; i < f.params.size(); i++) {
                if (i) out << ", ";
                out << f.params[i].first << " %" << f.params[i].first << " : " << nom_rir_type(f.params[i].second);
            }
            out << ") -> " << nom_rir_type(f.retour) << " {\n";
            for (auto &b : f.blocs) {
                out << b.nom << ":\n";
                for (auto &i : b.instrs) {
                    if (!i.dest.empty()) out << "  " << i.dest << " = ";
                    else out << "  ";
                    out << i.op;
                    for (auto &a : i.args) {
                        if (a.kind == RirOp::Const) out << " " << a.name;
                        else if (a.kind == RirOp::Temp) out << " " << a.name;
                        else if (a.kind == RirOp::Global) out << " $" << a.name;
                        else if (a.kind == RirOp::Label) out << " " << a.name;
                    }
                    if (!i.label_cible.empty()) out << " " << i.label_cible;
                    if (!i.label_cible2.empty()) out << " " << i.label_cible2;
                    if (!i.message.empty()) out << " \"" << i.message << "\"";
                    out << "\n";
                }
            }
            out << "}\n";
        }
        return out.str();
    }
};

/* ═══════════════════════════════════════════════════════════════════
   Interpréteur RIR (jalon M1b)

   Exécute un module RIR. Remplace progressivement l'interpréteur d'arbre.
   ═══════════════════════════════════════════════════════════════════ */

/* FFI : poignées de bibliothèques + distribution d'appels par types.
   Partagée entre l'interpréteur d'arbre (Interp) et le repli RIR (InterpRir) —
   les poignées sont stockées dans la map passée en argument. */
static void *ffi_poignee(Signature &sig, std::unordered_map<std::string, void*> &poignees) {
        std::string nom = !sig.dll_son.empty() ? sig.dll_son
                        : (!sig.dll.empty() ? sig.dll : std::string("c"));
        auto it = poignees.find(nom);
        if (it != poignees.end()) return it->second;
        std::vector<std::string> candidats;
        if (!sig.dll_son.empty() || nom.find('/') != std::string::npos) {
            candidats.push_back(nom);
        } else {
#ifdef _WIN32
            candidats.push_back(nom + ".dll");
            candidats.push_back("lib" + nom + ".dll");
            candidats.push_back(nom);
#else
            candidats.push_back("lib" + nom + ".so");
            candidats.push_back(nom + ".so");
            candidats.push_back(nom);
            candidats.push_back("lib" + nom + ".so.6");
            candidats.push_back("lib" + nom + ".so.7");
            candidats.push_back("lib" + nom + ".so.8");
#endif
        }
        void *h = nullptr;
        for (auto &c : candidats) {
            h = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (h) break;
        }
        if (!h) erreur_runtime("chargement de bibliothèque impossible (" + nom + ")");
        poignees[nom] = h;
        return h;
    }

static Valeur ffi_appeler(Signature &sig, std::vector<Valeur> &args,
                          std::unordered_map<std::string, void*> &poignees) {
    void *h = ffi_poignee(sig, poignees);
    const std::string &sym = sig.symbole.empty() ? sig.nom : sig.symbole;
    void *s = dlsym(h, sym.c_str());
    if (!s) erreur_runtime("symbole introuvable dans la bibliothèque (" + sym + ")");
    /* types C : entier→long long, flottant→double, booleen→int, chaine→char* */
    auto zarg = [&](size_t i) { return z_vers_ll(vers_z(args[i]).get_mpz_t()); };
    auto darg = [&](size_t i) { return vers_d(args[i]); };
    auto sent = [&](size_t i) { return sig.params[i].second.n == T_CHAINE; };
    switch (sig.retour.n) {
        case T_ENTIER: {
        switch (args.size()) {
            case 0: { auto fp = (long long (*)())s; return Valeur::de_z(z_depuis_ll(fp())); }
            case 1: {
                auto fp = (long long (*)(long long))s;
                return Valeur::de_z(z_depuis_ll(fp(zarg(0))));
            }
            case 2: {
                auto fp = (long long (*)(long long, long long))s;
                return Valeur::de_z(z_depuis_ll(fp(zarg(0), zarg(1))));
            }
            default: erreur_runtime("fonction externe : plus de 2 arguments entiers non supporté");
        }
        break;
        }
        case T_FLOTTANT: {
        switch (args.size()) {
            case 0: { auto fp = (double (*)())s; return Valeur::de_f(fp()); }
            case 1: {
                if (sent(0)) { auto fp = (double (*)(const char *))s; return Valeur::de_f(fp(args[0].s.c_str())); }
                if (sig.params[0].second.n == T_FLOTTANT) { auto fp = (double (*)(double))s; return Valeur::de_f(fp(darg(0))); }
                if (sig.params[0].second.n == T_BOOLEEN) { auto fp = (double (*)(int))s; return Valeur::de_f(fp(args[0].b ? 1 : 0)); }
                auto fp = (double (*)(long long))s;
                return Valeur::de_f(fp(zarg(0)));
            }
            case 2: {
                int p0 = sig.params[0].second.n, p1 = sig.params[1].second.n;
                if (p0 == T_FLOTTANT && p1 == T_FLOTTANT) { auto fp = (double (*)(double, double))s; return Valeur::de_f(fp(darg(0), darg(1))); }
                if (p0 == T_FLOTTANT) { auto fp = (double (*)(double, long long))s; return Valeur::de_f(fp(darg(0), zarg(1))); }
                if (p1 == T_FLOTTANT) { auto fp = (double (*)(long long, double))s; return Valeur::de_f(fp(zarg(0), darg(1))); }
                auto fp = (double (*)(long long, long long))s;
                return Valeur::de_f(fp(zarg(0), zarg(1)));
            }
            case 3: {
                auto fp = (double (*)(double, double, double))s;
                return Valeur::de_f(fp(darg(0), darg(1), darg(2)));
            }
            default: erreur_runtime("fonction externe flottante : arité non supportée");
        }
        break;
        }
        case T_BOOLEEN: {
        switch (args.size()) {
            case 0: { auto fp = (int (*)())s; return Valeur::de_b(fp() != 0); }
            case 1: { auto fp = (int (*)(long long))s; return Valeur::de_b(fp(zarg(0)) != 0); }
            default: erreur_runtime("fonction externe booléenne : arité non supportée");
        }
        break;
        }
        case T_CHAINE: {
        switch (args.size()) {
            case 0: { auto fp = (const char *(*)())s; const char *r = fp(); return Valeur::de_s(r ? r : ""); }
            case 1: {
                if (sent(0)) { auto fp = (const char *(*)(const char *))s; const char *r = fp(args[0].s.c_str()); return Valeur::de_s(r ? r : ""); }
                auto fp = (const char *(*)(long long))s;
                const char *r = fp(zarg(0));
                return Valeur::de_s(r ? r : "");
            }
            default: erreur_runtime("fonction externe texte : arité non supportée");
        }
        break;
        }
        default:
        erreur_runtime("type de retour externe non supporté");
    }
    return Valeur::vide();
    }

struct InterpRir {
    RirModule &mod;
    /* variables globales */
    std::unordered_map<std::string, Valeur> globaux;
    /* pile d'appels */
    struct Frame {
        RirFonction *fn;
        std::string bloc_courant;
        std::unordered_map<std::string, Valeur> vals;  /* SSA temps → Valeur */
        size_t pc;  /* index dans le bloc courant */
        Valeur ret_val;
    };
    std::vector<Frame> pile;
    std::unordered_map<std::string, void*> poignees;
    std::vector<void*> poignees_dyn;
    /* signatures du programme (pour retrouver les fonctions externes @dll/@sym) */
    Programme *prog = nullptr;

    explicit InterpRir(RirModule &m) : mod(m) {}

    /* ── fonctions dynamiques (dlopen à l'exécution) ──────────────── */
    Valeur dynamique(const std::string &nom, std::vector<Valeur> &args) {
        if (nom == "charger") {
            void *h = dlopen(args[0].s.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) erreur_runtime("chargement de bibliothèque impossible (" + args[0].s + ")");
            poignees_dyn.push_back(h);
            return Valeur::de_z(mpz_class((unsigned long)(uintptr_t)h));
        }
        if (nom == "fermer") {
            void *h = (void*)(uintptr_t)mpz_get_ui(args[0].z.get_mpz_t());
            dlclose(h);
            for (auto it = poignees_dyn.begin(); it != poignees_dyn.end(); ++it)
                if (*it == h) { poignees_dyn.erase(it); break; }
            return Valeur::vide();
        }
        if (nom == "appeler0" || nom == "appeler1" || nom == "appeler2") {
            void *h = (void*)(uintptr_t)mpz_get_ui(args[0].z.get_mpz_t());
            void *s = dlsym(h, args[1].s.c_str());
            if (!s) erreur_runtime("symbole introuvable dans la bibliothèque (" + args[1].s + ")");
            if (nom == "appeler0") {
                double (*fp)(void) = (double (*)(void))s;
                return Valeur::de_f(fp());
            }
            if (nom == "appeler1") {
                double a = vers_d(args[2]);
                if (args[2].ty == T_ENTIER) {
                    double (*fp)(long long) = (double (*)(long long))s;
                    return Valeur::de_f(fp(mpz_get_si(args[2].z.get_mpz_t())));
                }
                double (*fp)(double) = (double (*)(double))s;
                return Valeur::de_f(fp(a));
            }
            /* appeler2 : signatures doubles, mixtes */
            bool a_ent = args[2].ty == T_ENTIER, b_ent = args[3].ty == T_ENTIER;
            if (!a_ent && !b_ent) { auto fp = (double (*)(double, double))s; return Valeur::de_f(fp(vers_d(args[2]), vers_d(args[3]))); }
            if (a_ent && !b_ent)  { auto fp = (double (*)(long long, double))s; return Valeur::de_f(fp(mpz_get_si(vers_z(args[2]).get_mpz_t()), vers_d(args[3]))); }
            if (!a_ent && b_ent)  { auto fp = (double (*)(double, long long))s; return Valeur::de_f(fp(vers_d(args[2]), mpz_get_si(vers_z(args[3]).get_mpz_t()))); }
            { auto fp = (double (*)(long long, long long))s; return Valeur::de_f(fp(mpz_get_si(vers_z(args[2]).get_mpz_t()), mpz_get_si(vers_z(args[3]).get_mpz_t()))); }
        }
        erreur_runtime("fonction dynamique::" + nom + " inconnue");
        return Valeur::vide();
    }

    /* ── recherche de fonction ─────────────────────────────────────── */
    RirFonction *trouver_fonction(const std::string &nom) {
        for (auto &f : mod.fonctions) if (f.nom == nom) return &f;
        return nullptr;
    }

    /* ── recherche de bloc ─────────────────────────────────────────── */
    RirBloc *trouver_bloc(RirFonction &fn, const std::string &nom) {
        for (auto &b : fn.blocs) if (b.nom == nom) return &b;
        return nullptr;
    }

    /* ── exécution d'une instruction ───────────────────────────────── */
    /* retourne : 0=continue, 1=return (valeur dans *ret), 2=erreur */
    int exec_instr(RirInstr &i, Frame &frame, RirFonction &fn, Valeur *ret) {
        /* évaluer les arguments (temporaire → Valeur) */
        auto get_val = [&](const RirOp &op) -> Valeur {
            if (op.kind == RirOp::Const) {
                /* chercher dans les constantes du module */
                for (auto &c : mod.consts) if (c.nom == op.name) {
                    if (c.ty == RirType::Z) { mpz_class z; mpz_set_str(z.get_mpz_t(), c.texte.c_str(), 10); return Valeur::de_z(z); }
                    if (c.ty == RirType::Q) {
                        /* parse p/q */
                        size_t slash = c.texte.find('/');
                        if (slash != std::string::npos) {
                            mpz_class num, den;
                            mpz_set_str(num.get_mpz_t(), c.texte.substr(0, slash).c_str(), 10);
                            mpz_set_str(den.get_mpz_t(), c.texte.substr(slash+1).c_str(), 10);
                            return Valeur::de_q(mpq_class(num, den));
                        }
                        mpz_class z; mpz_set_str(z.get_mpz_t(), c.texte.c_str(), 10);
                        return Valeur::de_q(mpq_class(z));
                    }
                    if (c.ty == RirType::F) return Valeur::de_f(std::stod(c.texte));
                    if (c.ty == RirType::B) return Valeur::de_b(c.texte == "vrai");
                    if (c.ty == RirType::S) return Valeur::de_s(c.texte);
                    return Valeur::vide();
                }
                /* constante inline (pas dans la table) */
                if (op.name.rfind("@c", 0) != 0) {
                    /* littéral non chiffré : booléen selon le type déclaré, sinon chaîne */
                    if (op.ty == RirType::B) return Valeur::de_b(op.name == "vrai");
                    return Valeur::de_s(op.name);
                }
                if (op.name.rfind("@c", 0) == 0) {
                    std::string txt = op.name.substr(2);
                    if (txt.find('/') != std::string::npos) {
                        size_t sl = txt.find('/');
                        mpz_class num, den;
                        mpz_set_str(num.get_mpz_t(), txt.substr(0, sl).c_str(), 10);
                        mpz_set_str(den.get_mpz_t(), txt.substr(sl+1).c_str(), 10);
                        return Valeur::de_q(mpq_class(num, den));
                    }
                    /* essayer entier */
                    if (txt.find('.') == std::string::npos) {
                        mpz_class z;
                        if (mpz_set_str(z.get_mpz_t(), txt.c_str(), 10) == 0)
                            return Valeur::de_z(z);
                    }
                    try { return Valeur::de_f(std::stod(txt)); } catch (...) {}
                    if (txt == "vrai") return Valeur::de_b(true);
                    if (txt == "faux") return Valeur::de_b(false);
                    return Valeur::de_s(txt);
                }
                return Valeur::vide();
            }
            if (op.kind == RirOp::Temp) {
                auto it = frame.vals.find(op.name);
                if (it != frame.vals.end()) return it->second;
                /* chercher dans les globaux */
                auto g = globaux.find(op.name);
                if (g != globaux.end()) return g->second;
                // v3.13.1 : lecture d'une variable jamais affectée (assignation
                // uniquement atteinte sur un chemin non suivi) — l'ancienne
                // réponse « vide » affichait du silence à la place de l'erreur
                // que Rust et Zig produisent. Trouvé par le testeur génératif.
                std::string nu = op.name;
                if (!nu.empty() && nu[0] == '%') nu = nu.substr(1);
                erreur_runtime("variable '" + nu + "' utilisée sans valeur");
            }
            if (op.kind == RirOp::Global) {
                auto g = globaux.find(op.name);
                if (g != globaux.end()) return g->second;
                std::string nu = op.name;
                if (!nu.empty() && nu[0] == '%') nu = nu.substr(1);
                erreur_runtime("variable '" + nu + "' utilisée sans valeur");
            }
            return Valeur::vide();
        };

        /* ── arithmétique ──────────────────────────────────────────── */
        if (i.op.rfind("cst.", 0) == 0) {
            frame.vals[i.dest] = get_val(i.args[0]);
            return 0;
        }
        if (i.op.rfind("add.", 0) == 0 || i.op.rfind("sub.", 0) == 0 ||
            i.op.rfind("mul.", 0) == 0) {
            Valeur g = get_val(i.args[0]), d = get_val(i.args[1]);
            char type = i.op.back();
            if (type == 'z') {
                if (i.op.rfind("add", 0) == 0) {
                    mpz_class r; mpz_add(r.get_mpz_t(), vers_z(g).get_mpz_t(), vers_z(d).get_mpz_t()); frame.vals[i.dest] = Valeur::de_z(r);
                } else if (i.op.rfind("sub", 0) == 0) {
                    mpz_class r; mpz_sub(r.get_mpz_t(), vers_z(g).get_mpz_t(), vers_z(d).get_mpz_t()); frame.vals[i.dest] = Valeur::de_z(r);
                } else {
                    mpz_class r; mpz_mul(r.get_mpz_t(), vers_z(g).get_mpz_t(), vers_z(d).get_mpz_t()); frame.vals[i.dest] = Valeur::de_z(r);
                }
            } else if (type == 'q') {
                if (i.op.rfind("add", 0) == 0) frame.vals[i.dest] = Valeur::de_q(vers_q(g) + vers_q(d));
                else if (i.op.rfind("sub", 0) == 0) frame.vals[i.dest] = Valeur::de_q(vers_q(g) - vers_q(d));
                else frame.vals[i.dest] = Valeur::de_q(vers_q(g) * vers_q(d));
            } else if (type == 'f') {
                if (i.op.rfind("add", 0) == 0) frame.vals[i.dest] = Valeur::de_f(vers_d(g) + vers_d(d));
                else if (i.op.rfind("sub", 0) == 0) frame.vals[i.dest] = Valeur::de_f(vers_d(g) - vers_d(d));
                else frame.vals[i.dest] = Valeur::de_f(vers_d(g) * vers_d(d));
            } else if (type == 's') {
                if (i.op.rfind("add", 0) == 0) frame.vals[i.dest] = Valeur::de_s(g.s + d.s);
            }
            return 0;
        }
        if (i.op.rfind("divf", 0) == 0) {
            /* division euclidienne : flottant → plancher, rationnel → entier, entier → tronqué */
            Valeur g = get_val(i.args[0]), d = get_val(i.args[1]);
            if (g.ty == T_FLOTTANT || d.ty == T_FLOTTANT) {
                double b = vers_d(d);
                if (b == 0.0) erreur_runtime("division euclidienne par zéro");
                frame.vals[i.dest] = Valeur::de_f(std::floor(vers_d(g) / b));
            } else if (g.ty == T_RATIONNEL || d.ty == T_RATIONNEL) {
                mpq_class dq = vers_q(d);
                if (sgn_q(dq) == 0) erreur_runtime("division euclidienne par zéro");
                mpq_class r = vers_q(g) / dq;
                mpz_class f2;
                mpz_fdiv_q(f2.get_mpz_t(), r.get_num().get_mpz_t(), r.get_den().get_mpz_t());
                frame.vals[i.dest] = Valeur::de_z(f2);
            } else {
                mpz_class dz = vers_z(d);
                if (mpz_sgn(dz.get_mpz_t()) == 0) erreur_runtime("division euclidienne par zéro");
                mpz_class r; mpz_fdiv_q(r.get_mpz_t(), vers_z(g).get_mpz_t(), dz.get_mpz_t());
                frame.vals[i.dest] = Valeur::de_z(r);
            }
            return 0;
        }
        if (i.op.rfind("div", 0) == 0) {
            /* division exacte : flottant → flottant, sinon toujours rationnel */
            Valeur g = get_val(i.args[0]), d = get_val(i.args[1]);
            if (g.ty == T_FLOTTANT || d.ty == T_FLOTTANT) {
                if (vers_d(d) == 0.0) erreur_runtime("division par zéro");
                frame.vals[i.dest] = Valeur::de_f(vers_d(g) / vers_d(d));
            } else {
                mpq_class dq = vers_q(d);
                if (sgn_q(dq) == 0) erreur_runtime("division par zéro");
                frame.vals[i.dest] = Valeur::de_q(vers_q(g) / dq);
            }
            return 0;
        }
        if (i.op.rfind("pow", 0) == 0) {
            /* miroir de l'évaluateur d'arbre : flottant → pow double ;
               sinon exposant entier (tronqué si rationnel), q^e et z^négatif → q */
            Valeur g = get_val(i.args[0]), d = get_val(i.args[1]);
            if (d.ty == T_FLOTTANT) frame.vals[i.dest] = Valeur::de_f(std::pow(vers_d(g), d.f));
            else if (g.ty == T_FLOTTANT) frame.vals[i.dest] = Valeur::de_f(std::pow(g.f, vers_d(d)));
            else {
                mpz_class expo = vers_z(d);
                if (!mpz_fits_slong_p(expo.get_mpz_t()))
                    erreur_runtime("exposant démesuré (la puissance exploserait au-delà de tout calcul)");
                long e2 = mpz_get_si(expo.get_mpz_t());
                if (g.ty == T_RATIONNEL) {
                    mpq_class r; rose_pow_rationnel(r, vers_q(g), e2);
                    frame.vals[i.dest] = Valeur::de_q(r);
                } else {
                    mpz_class base = vers_z(g);
                    if (e2 >= 0) {
                        mpz_class r;
                        mpz_pow_ui(r.get_mpz_t(), base.get_mpz_t(), (unsigned long)e2);
                        frame.vals[i.dest] = Valeur::de_z(r);
                    } else {
                        if (mpz_sgn(base.get_mpz_t()) == 0) erreur_runtime("0^exposant négatif");
                        mpq_class r;
                        rose_pow_rationnel(r, mpq_class(base), e2);
                        frame.vals[i.dest] = Valeur::de_q(r);
                    }
                }
            }
            return 0;
        }
        if (i.op.rfind("neg.", 0) == 0) {
            Valeur v = get_val(i.args[0]);
            if (v.ty == T_ENTIER) { mpz_class r = vers_z(v); mpz_neg(r.get_mpz_t(), r.get_mpz_t()); frame.vals[i.dest] = Valeur::de_z(r); }
            else if (v.ty == T_RATIONNEL) frame.vals[i.dest] = Valeur::de_q(-vers_q(v));
            else frame.vals[i.dest] = Valeur::de_f(-vers_d(v));
            return 0;
        }
        if (i.op.rfind("cmp.", 0) == 0) {
            Valeur g = get_val(i.args[0]), d = get_val(i.args[1]);
            bool r = false;
            if (i.op == "cmp.approx") {
                /* ≈ / ?= / =~ : tolérance 1e-9 relative sur flottants (cible C) */
                if (g.ty == T_FLOTTANT || d.ty == T_FLOTTANT) {
                    double a = vers_d(g), b = vers_d(d);
                    double scale = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
                    if (scale < 1.0) scale = 1.0;
                    r = std::fabs(a - b) <= 1e-9 * scale;
                } else {
                    r = cmp_vals(g, d) == 0;
                }
            }
            else if (i.op.rfind("cmp.eq", 0) == 0) r = rose_valeur_egale(g, d);
            else if (i.op.rfind("cmp.ne", 0) == 0) r = !rose_valeur_egale(g, d);
            else if (i.op.rfind("cmp.lt", 0) == 0) r = cmp_vals(g, d) < 0;
            else if (i.op.rfind("cmp.le", 0) == 0) r = cmp_vals(g, d) <= 0;
            else if (i.op.rfind("cmp.gt", 0) == 0) r = cmp_vals(g, d) > 0;
            else if (i.op.rfind("cmp.ge", 0) == 0) r = cmp_vals(g, d) >= 0;
            frame.vals[i.dest] = Valeur::de_b(r);
            return 0;
        }
        if (i.op.rfind("and.", 0) == 0 || i.op.rfind("or.", 0) == 0) {
            /* et / ou booléens (mêmes valeurs de vérité que l'arbre) */
            auto bool_de = [](const Valeur &v) {
                if (v.ty == T_BOOLEEN) return v.b;
                if (v.ty == T_ENTIER) return mpz_sgn(v.z.get_mpz_t()) != 0;
                if (v.ty == T_FLOTTANT) return v.f != 0.0;
                if (v.ty == T_RATIONNEL) return sgn_q(v.q) != 0;
                return false;
            };
            bool g = bool_de(get_val(i.args[0])), d = bool_de(get_val(i.args[1]));
            frame.vals[i.dest] = Valeur::de_b(i.op.rfind("and", 0) == 0 ? (g && d) : (g || d));
            return 0;
        }
        if (i.op == "not.b") {
            frame.vals[i.dest] = Valeur::de_b(!get_val(i.args[0]).b);
            return 0;
        }
        if (i.op.rfind("rem.z", 0) == 0) {
            Valeur g = get_val(i.args[0]), d = get_val(i.args[1]);
            mpz_class dz = vers_z(d);
            if (mpz_sgn(dz.get_mpz_t()) == 0) erreur_runtime("modulo par zéro");
            mpz_class r; mpz_fdiv_r(r.get_mpz_t(), vers_z(g).get_mpz_t(), dz.get_mpz_t());
            frame.vals[i.dest] = Valeur::de_z(r);
            return 0;
        }
        if (i.op.rfind("call", 0) == 0) {
            /* call $nom args... ou call @builtin.nom args... */
            std::string nom = i.args[0].name;
            /* extraire le nom de la fonction */
            if (nom.rfind("$", 0) == 0) nom = nom.substr(1);
            std::vector<Valeur> args;
            for (size_t j = 1; j < i.args.size(); j++) args.push_back(get_val(i.args[j]));
            if (nom == "factorielle") {
                frame.vals[i.dest] = Valeur::de_z(rose_factorielle(vers_z(args[0])));
                return 0;
            }
            if (nom == "afficher") {
                afficher(args[0]); frame.vals[i.dest] = Valeur::vide(); return 0;
            }
            if (nom == "concatener") {
                std::vector<Valeur> args2;
                for (size_t j = 1; j < i.args.size(); j++) args2.push_back(get_val(i.args[j]));
                if (!args2.empty() && args2[0].ty == T_LISTE && args2[0].l && !args2[0].l->empty()) {
                    std::string r;
                    for (const Valeur &e : *args2[0].l) {
                        if (e.ty != T_CHAINE) { erreur_runtime("concatener attend une liste de chaînes"); }
                        r += e.s;
                    }
                    frame.vals[i.dest] = Valeur::de_s(r);
                    return 0;
                }
                frame.vals[i.dest] = Valeur::de_s("");
                return 0;
            }
            if (nom == "joindre") {
                std::vector<Valeur> args2;
                for (size_t j = 1; j < i.args.size(); j++) args2.push_back(get_val(i.args[j]));
                if (args2.size() >= 2 && args2[0].ty == T_LISTE && args2[0].l) {
                    auto &vec = *args2[0].l;
                    vec.push_back(args2[1]);
                }
                frame.vals[i.dest] = Valeur::vide();
                return 0;
            }
            if (nom == "partie_entiere") {
                Valeur v = i.args.size() > 1 ? get_val(i.args[1]) : Valeur::vide();
                if (v.ty == T_ENTIER || v.ty == T_BOOLEEN) frame.vals[i.dest] = v;
                else if (v.ty == T_RATIONNEL) {
                    mpz_class r; mpz_fdiv_q(r.get_mpz_t(), vers_q(v).get_num().get_mpz_t(), vers_q(v).get_den().get_mpz_t());
                    frame.vals[i.dest] = Valeur::de_z(r);
                } else if (v.ty == T_FLOTTANT) {
                    mpz_class r; mpz_set_d(r.get_mpz_t(), std::floor(vers_d(v)));
                    frame.vals[i.dest] = Valeur::de_z(r);
                } else if (v.ty == T_CHAINE) {
                    try { mpz_class r; mpz_set_d(r.get_mpz_t(), std::floor(std::stod(v.s))); frame.vals[i.dest] = Valeur::de_z(r); return 0; } catch (...) {}
                }
                return 0;
            }
            /* conversions explicites */
            if (nom == "entier" || nom == "rationnel" || nom == "flottant" || nom == "chaine") {
                Valeur v = i.args.size() > 1 ? get_val(i.args[1]) : Valeur::vide();
                if (nom == "entier") {
                    if (v.ty == T_CHAINE) { mpz_class r; chaine_vers_entier(r, v.s); frame.vals[i.dest] = Valeur::de_z(r); return 0; }
                    frame.vals[i.dest] = convertir(v, T_ENTIER); return 0;
                }
                if (nom == "rationnel") { frame.vals[i.dest] = convertir(v, T_RATIONNEL); return 0; }
                if (nom == "flottant") {
                    if (v.ty == T_CHAINE) frame.vals[i.dest] = Valeur::de_f(chaine_vers_flottant(v.s));
                    else frame.vals[i.dest] = convertir(v, T_FLOTTANT);
                    return 0;
                }
                if (nom == "chaine") { frame.vals[i.dest] = Valeur::de_s(chaine_de(v)); return 0; }
            }
            /* fonction utilisateur */
            RirFonction *rf = trouver_fonction("$" + nom);
            if (!rf && prog) {
                /* fonction externe @dll/@sym : FFI via dlopen/dlsym */
                auto sit = prog->signatures.find(nom);
                if (sit != prog->signatures.end() && sit->second && !sit->second->corps &&
                    (!sit->second->dll.empty() || !sit->second->dll_son.empty() ||
                     !sit->second->symbole.empty())) {
                    Valeur res = ffi_appeler(*sit->second, args, poignees);
                    if (!i.dest.empty()) frame.vals[i.dest] = std::move(res);
                    return 0;
                }
            }
            if (!rf && (nom == "charger" || nom == "fermer" ||
                        nom == "appeler0" || nom == "appeler1" || nom == "appeler2")) {
                Valeur res = dynamique(nom, args);
                if (!i.dest.empty()) frame.vals[i.dest] = std::move(res);
                return 0;
            }
            if (!rf) {
                /* builtin stdlib */
                std::vector<Valeur> args2;
                for (size_t j = 1; j < i.args.size(); j++) args2.push_back(get_val(i.args[j]));
                Valeur res = stdlib(nom, args2, i.args.empty() ? 0 : 0);
                if (!i.dest.empty()) frame.vals[i.dest] = res;
                return 0;
            }
            /* appeler récursivement */
            /* ... (simplified: push frame, execute, pop) */
            Frame newframe; newframe.fn = rf; newframe.bloc_courant = rf->blocs[0].nom; newframe.pc = 0;
            /* bind params */
            for (size_t j = 0; j < rf->params.size() && j < args.size(); j++) {

                newframe.vals["%" + rf->params[j].first] = args[j];
            }
            newframe.ret_val = Valeur::vide();
            /* execute recursively */
            executer_fonction(*rf, newframe);
            Valeur res = newframe.ret_val;
            if (!i.dest.empty()) frame.vals[i.dest] = res;
            return 0;
        }
        if (i.op == "index") {
            Valeur base = get_val(i.args[0]);
            Valeur cle = get_val(i.args[1]);
            if (base.ty == T_TABLE) {
                for (const auto &kv : *base.t)
                    if (rose_valeur_egale(kv.first, cle)) { frame.vals[i.dest] = kv.second; return 0; }
                erreur_runtime("clé absente de la table");
            }
            if (base.ty == T_LISTE) {
                mpz_class idx = vers_z(cle);
                size_t taille = base.l->size();
                if (!mpz_fits_ulong_p(idx.get_mpz_t()) || mpz_cmp_ui(idx.get_mpz_t(), taille) >= 0)
                    erreur_runtime("index hors des limites de la liste");
                frame.vals[i.dest] = (*base.l)[(size_t)mpz_get_ui(idx.get_mpz_t())];
                return 0;
            }
            if (base.ty == T_CHAINE) {
                mpz_class idx = vers_z(cle);
                size_t taille = utf8_len(base.s);
                if (!mpz_fits_ulong_p(idx.get_mpz_t()) || mpz_cmp_ui(idx.get_mpz_t(), taille) >= 0)
                    erreur_runtime("index hors des limites de la chaîne");
                frame.vals[i.dest] = Valeur::de_s(utf8_char_str(base.s, (size_t)mpz_get_ui(idx.get_mpz_t())));
                return 0;
            }
            erreur_runtime("index sur une valeur qui n'est pas une liste");
            return 0;
        }
        if (i.op == "liste.new") {
            frame.vals[i.dest] = Valeur::de_l(std::make_shared<std::vector<Valeur>>());
            return 0;
        }
        if (i.op == "liste.push") {
            Valeur listv = get_val(i.args[0]);
            Valeur elem = get_val(i.args[1]);
            if (listv.ty != T_LISTE || !listv.l) erreur_runtime("liste.push attend une liste");
            listv.l->push_back(elem);
            frame.vals[i.dest] = listv;
            return 0;
        }
        if (i.op == "liste.get") {
            Valeur listv = get_val(i.args[0]);
            mpz_class idx = vers_z(get_val(i.args[1]));
            if (listv.ty != T_LISTE || !listv.l) erreur_runtime("liste.get attend une liste");
            size_t taille = listv.l->size();
            if (!mpz_fits_ulong_p(idx.get_mpz_t()) || mpz_cmp_ui(idx.get_mpz_t(), taille) >= 0)
                erreur_runtime("index hors des limites de la liste");
            frame.vals[i.dest] = (*listv.l)[(size_t)mpz_get_ui(idx.get_mpz_t())];
            return 0;
        }
        if (i.op == "liste.len") {
            Valeur listv = get_val(i.args[0]);
            if (listv.ty != T_LISTE || !listv.l) erreur_runtime("liste.len attend une liste");
            frame.vals[i.dest] = Valeur::de_z(mpz_class((unsigned long)listv.l->size()));
            return 0;
        }
        if (i.op == "lire") {
            /* message = type cible (entier/flottant/rationnel), choisi par le
               vérificateur selon la variable ; args[0] = la variable cible */
            int ty = i.message.empty() ? T_ENTIER : std::atoi(i.message.c_str());
            frame.vals[i.dest] = lire_selon(ty);
            return 0;
        }
        if (i.op == "affect") {
            Valeur v = get_val(i.args[0]);
            std::string nom_var = i.message;
            /* affecter dans le frame courant ou global */
            auto it = frame.vals.find("%" + nom_var);
            if (it != frame.vals.end()) { it->second = convertir(v, it->second.ty); }
            else {
                auto g = globaux.find(nom_var);
                if (g != globaux.end()) g->second = convertir(v, g->second.ty);
                else frame.vals["%" + nom_var] = v;
            }
            return 0;
        }
        if (i.op == "afficher") {
            afficher(get_val(i.args[0]));
            return 0;
        }
        if (i.op == "br") {
            frame.bloc_courant = i.label_cible;
            frame.pc = 0;
            return 0;
        }
        if (i.op == "br.cond") {
            Valeur cv = get_val(i.args[0]);
            bool cond = cv.b;
            frame.bloc_courant = cond ? i.label_cible : i.label_cible2;
            frame.pc = 0;
            return 0;
        }
        if (i.op.rfind("ret", 0) == 0) {

            Valeur v = i.args.empty() ? Valeur::vide() : get_val(i.args[0]);
            /* signaler le retour */
            frame.ret_val = v;
            return 1;  /* 1 = return */
        }
        if (i.op == "erreur") {
            erreur_runtime(i.message);
        }
        return 0;
    }

    /* ── exécution d'une fonction RIR ──────────────────────────────── */
    int executer_fonction(RirFonction &fn, Frame &frame) {
        while (true) {
            RirBloc *bloc = trouver_bloc(fn, frame.bloc_courant);
            if (!bloc) { erreur_runtime("bloc introuvable : " + frame.bloc_courant); return 1; }
            if (frame.pc >= bloc->instrs.size()) {
                /* fin de bloc sans saut : on tombe dans le bloc suivant
                   (ordre linéaire du vecteur de blocs), sinon sortie */
                size_t idx = fn.blocs.size();
                for (size_t k = 0; k < fn.blocs.size(); k++)
                    if (fn.blocs[k].nom == frame.bloc_courant) { idx = k; break; }
                if (idx + 1 >= fn.blocs.size()) return 0;
                frame.bloc_courant = fn.blocs[idx + 1].nom;
                frame.pc = 0;
                continue;
            }
            RirInstr &i = bloc->instrs[frame.pc];
            frame.pc++;
            int rc = exec_instr(i, frame, fn, nullptr);
            if (rc == 1) return 1;  /* return */
            if (rc != 0) return rc;
            /* si br/br.cond a changé le bloc, réinitialiser pc */
            /* (géré par le changement de bloc_courant) */
            RirBloc *nouveau = trouver_bloc(fn, frame.bloc_courant);
            if (nouveau != bloc) {
                frame.pc = 0;
            }
        }
    }

    /* ── point d'entrée ────────────────────────────────────────────── */
    void executer() {
        RirFonction *principal = trouver_fonction("$principal");
        if (!principal) { erreur_runtime("pas de fonction $principal"); return; }
        Frame frame;
        frame.fn = principal;
        frame.bloc_courant = principal->blocs[0].nom;
        frame.pc = 0;
        executer_fonction(*principal, frame);
    }
};

static std::string emettre_rir(Programme &prog) {
    RirModule mod;
    EmetteurRir em(mod);
    for (auto &kv : prog.signatures) {
        if (kv.second->corps) em.emettre_fonction(*kv.second);
    }
    em.emettre_principal(prog);
    return em.dump();
}

struct Interp {
    Programme &prog;
    std::unordered_map<std::string, Valeur> globaux;
    struct Frame { std::unordered_map<std::string, Valeur> vars; };
    std::vector<Frame> trames;
    std::unordered_map<std::string, void*> poignees;
    std::vector<void*> poignees_dyn;

    explicit Interp(Programme &p) : prog(p) { trames.emplace_back(); }

    /* ── variables ─────────────────────────────────────────────────── */
    Valeur *cible_var(const std::string &nom) {
        auto &f = trames.back().vars;
        auto it = f.find(nom);
        if (it != f.end()) return &it->second;
        auto g = globaux.find(nom);
        if (g != globaux.end()) return &g->second;
        return nullptr;
    }
    void affecter(const std::string &nom, Valeur v) {
        auto &f = trames.back().vars;
        auto it = f.find(nom);
        if (it != f.end()) { it->second = convertir(std::move(v), it->second.ty); return; }
        auto g = globaux.find(nom);
        if (g != globaux.end()) { g->second = convertir(std::move(v), g->second.ty); return; }
        f[nom] = std::move(v);
    }

    /* ── appels ────────────────────────────────────────────────────── */
    struct Garde {
        std::vector<Frame> &t;
        explicit Garde(std::vector<Frame> &tt) : t(tt) { t.emplace_back(); }
        ~Garde() { t.pop_back(); }
    };

    Valeur appeler(Signature &sig, std::vector<Valeur> args) {
        if (!sig.corps) return appeler_externe(sig, args);
        for (size_t i = 0; i < sig.params.size(); i++)
            args[i] = convertir(std::move(args[i]), sig.params[i].second.n);
        Garde garde(trames);
        for (size_t i = 0; i < sig.params.size(); i++)
            trames.back().vars[sig.params[i].first] = std::move(args[i]);
        Valeur res = Valeur::vide();
        try {
            for (PI &i : sig.corps->instrs) exec(i);
        } catch (RetourSignale &r) {
            res = convertir(std::move(r.v), sig.retour.n);
        }
        return res;
    }

    void *poignee_bibliotheque(Signature &sig) { return ffi_poignee(sig, poignees); }
    Valeur appeler_externe(Signature &sig, std::vector<Valeur> &args) {
        return ffi_appeler(sig, args, poignees);
    }

    /* ── instructions ──────────────────────────────────────────────── */
    void exec(PI instr) {
        switch (instr->k) {
            case IK::Afficher: afficher(eval(instr->expr)); return;
            case IK::Lire: {
                Valeur v = lire_selon(instr->type_read);
                affecter(instr->nom, std::move(v));
                return;
            }
            case IK::Affectation: affecter(instr->nom, eval(instr->expr)); return;
            case IK::Si: {
                if (eval(instr->test).b) exec_bloc(instr->alors);
                else if (instr->sinon) exec_bloc(instr->sinon);
                return;
            }
            case IK::TantQue:
                while (eval(instr->test).b) exec_bloc(instr->corps);
                return;
            case IK::Pour: {
                mpz_class debut = vers_z(eval(instr->debut));
                mpz_class fin = vers_z(eval(instr->fin));
                trames.back().vars[instr->nom] = Valeur::de_z(debut);
                try {
                    for (;;) {
                        Valeur &cur = trames.back().vars[instr->nom];
                        if (mpz_cmp(cur.z.get_mpz_t(), fin.get_mpz_t()) >= 0) break;
                        for (PI &s : instr->corps->instrs) exec(s);
                        mpz_add_ui(trames.back().vars[instr->nom].z.get_mpz_t(),
                                   trames.back().vars[instr->nom].z.get_mpz_t(), 1);
                    }
                } catch (...) {
                    trames.back().vars.erase(instr->nom);
                    throw;
                }
                trames.back().vars.erase(instr->nom);
                return;
            }
            case IK::Retour:
                throw RetourSignale{instr->expr ? eval(instr->expr) : Valeur::vide()};
            case IK::Expr: eval(instr->expr); return;
            case IK::Bloc:
                for (PI &s : instr->instrs) exec(s);
                return;
        }
    }
    void exec_bloc(PB b) { for (PI &i : b->instrs) exec(i); }

    /* ── expressions ───────────────────────────────────────────────── */
    Valeur eval(PE e) {
        switch (e->k) {
            case NK::Litteral: {
                if (e->flag) return Valeur::de_f(std::strtod(e->texte.c_str(), nullptr));
                if (e->texte.find('/') != std::string::npos)
                    return Valeur::de_q(mpq_class(e->texte));
                mpz_class z;
                if (mpz_set_str(z.get_mpz_t(), e->texte.c_str(), 10))
                    erreur_runtime("constante invalide (" + e->texte + ")");
                return Valeur::de_z(z);
            }
            case NK::ChaineLit: return Valeur::de_s(e->texte);
            case NK::BooleenLit: return Valeur::de_b(e->flag);
            case NK::Var: {
                Valeur *v = cible_var(e->texte);
                if (!v) erreur_runtime("variable '" + e->texte + "' utilisée sans valeur");
                return *v;                          /* les listes partagent le stockage */
            }
            case NK::ListeLit: {
                auto l = std::make_shared<std::vector<Valeur>>();
                for (PE &a : e->args) l->push_back(eval(a));
                return Valeur::de_l(l);
            }
            case NK::Index: {
                Valeur base = eval(e->a);
                if (base.ty != T_LISTE && base.ty != T_CHAINE && base.ty != T_TABLE)
                    erreur_runtime("index sur une valeur qui n'est pas une liste");
                if (base.ty == T_TABLE) {
                    Valeur cle = eval(e->b);
                    for (const auto &kv : *base.t)
                        if (rose_valeur_egale(kv.first, cle)) return kv.second;
                    erreur_runtime("clé absente de la table");
                    return Valeur::vide();
                }
                mpz_class idx = vers_z(eval(e->b));
                size_t taille = (base.ty == T_LISTE) ? base.l->size() : utf8_len(base.s);
                if (!mpz_fits_ulong_p(idx.get_mpz_t())
                        || mpz_cmp_ui(idx.get_mpz_t(), taille) >= 0)
                    erreur_runtime(base.ty == T_LISTE ? "index hors des limites de la liste"
                                                      : "index hors des limites de la chaîne");
                if (base.ty == T_LISTE) return (*base.l)[(size_t)mpz_get_ui(idx.get_mpz_t())];
                return Valeur::de_s(utf8_char_str(base.s, (size_t)mpz_get_ui(idx.get_mpz_t())));
            }
            case NK::Unaire: {
                Valeur v = eval(e->a);
                if (e->texte == "non") return Valeur::de_b(!v.b);
                if (v.ty == T_ENTIER) return Valeur::de_z(-v.z);
                if (v.ty == T_RATIONNEL) return Valeur::de_q(-v.q);
                if (v.ty == T_FLOTTANT) return Valeur::de_f(-v.f);
                if (v.ty == T_BOOLEEN) return Valeur::de_z(v.b ? -1 : 0);
                erreur_runtime("opérateur unaire '-' sur un " + chaine_nom(v.ty));
                return Valeur::vide();
            }
            case NK::Factorielle: {
                Valeur v = eval(e->a);
                return Valeur::de_z(rose_factorielle(vers_z(v)));
            }
            case NK::Binaire: return binaire(*e);
            case NK::Appel: return appel(*e);
        }
        return Valeur::vide();
    }

    static std::string chaine_nom(int ty) {
        return Ty(ty).nom();
    }

    Valeur binaire(E &e) {
        const std::string &op = e.texte;
        if (op == "et") {
            if (!eval(e.a).b) return Valeur::de_b(false);
            return Valeur::de_b(eval(e.b).b);
        }
        if (op == "ou") {
            if (eval(e.a).b) return Valeur::de_b(true);
            return Valeur::de_b(eval(e.b).b);
        }
        Valeur g = eval(e.a), d = eval(e.b);
        /* comparaisons */
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (op == "==" || op == "!=") {
                if (g.ty == T_LISTE || d.ty == T_LISTE) {
                    bool eg = (g.ty == T_LISTE && d.ty == T_LISTE && g.l->size() == d.l->size());
                    if (eg)
                        for (size_t i = 0; i < g.l->size(); i++)
                            if (!rose_valeur_egale((*g.l)[i], (*d.l)[i])) { eg = false; break; }
                    bool r = (op == "==") ? eg : !eg;
                    return Valeur::de_b(r);
                }
                if (g.ty == T_TABLE || d.ty == T_TABLE) {
                    bool eg = rose_valeur_egale(g, d);
                    bool r = (op == "==") ? eg : !eg;
                    return Valeur::de_b(r);
                }
                if ((g.ty == T_CHAINE) != (d.ty == T_CHAINE))
                    return Valeur::de_b(op == "!=");      /* chaine vs non-chaine : jamais égales */
            }
            int c = cmp_vals(g, d);
            bool r = op == "==" ? c == 0 : op == "!=" ? c != 0
                   : op == "<" ? c < 0 : op == "<=" ? c <= 0
                   : op == ">" ? c > 0 : c >= 0;
            return Valeur::de_b(r);
        }
        if (op == "≈" || op == "?=" || op == "=~") {
            bool r;
            if (g.ty == T_FLOTTANT || d.ty == T_FLOTTANT) {
                double a = vers_d(g), b = vers_d(d);
                double scale = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
                if (scale < 1.0) scale = 1.0;
                r = std::fabs(a - b) <= 1e-9 * scale;
            } else {
                r = cmp_vals(g, d) == 0;
            }
            return Valeur::de_b(r);
        }
        if (op.rfind("add.", 0) == 0 && g.ty == T_CHAINE && d.ty == T_CHAINE)
            return Valeur::de_s(g.s + d.s);
        int m = std::max(rnk(g), rnk(d));
        if (op == "+") {
            if (m == 3) return Valeur::de_f(vers_d(g) + vers_d(d));
            if (m == 2) return Valeur::de_q(vers_q(g) + vers_q(d));
            return Valeur::de_z(vers_z(g) + vers_z(d));
        }
        if (op == "-") {
            if (m == 3) return Valeur::de_f(vers_d(g) - vers_d(d));
            if (m == 2) return Valeur::de_q(vers_q(g) - vers_q(d));
            return Valeur::de_z(vers_z(g) - vers_z(d));
        }
        if (op == "*") {
            if (m == 3) return Valeur::de_f(vers_d(g) * vers_d(d));
            if (m == 2) return Valeur::de_q(vers_q(g) * vers_q(d));
            return Valeur::de_z(vers_z(g) * vers_z(d));
        }
        if (op == "/") {
            if (m == 3) {
                if (vers_d(d) == 0.0) erreur_runtime("division par zéro");
                return Valeur::de_f(vers_d(g) / vers_d(d));
            }
            mpq_class dq = vers_q(d);
            if (sgn_q(dq) == 0) erreur_runtime("division par zéro");
            return Valeur::de_q(vers_q(g) / dq);
        }
        if (op == "div") {
            if (g.ty == T_FLOTTANT || d.ty == T_FLOTTANT) {
                double b = vers_d(d);
                if (b == 0.0) erreur_runtime("division euclidienne par zéro");
                return Valeur::de_f(std::floor(vers_d(g) / b));
            }
            if (g.ty == T_RATIONNEL || d.ty == T_RATIONNEL) {
                mpq_class dq = vers_q(d);
                if (sgn_q(dq) == 0) erreur_runtime("division euclidienne par zéro");
                mpq_class r = vers_q(g) / dq;
                mpz_class f;
                mpz_fdiv_q(f.get_mpz_t(), r.get_num().get_mpz_t(), r.get_den().get_mpz_t());
                return Valeur::de_z(f);
            }
            mpz_class dz = vers_z(d);
            if (mpz_sgn(dz.get_mpz_t()) == 0) erreur_runtime("division euclidienne par zéro");
            mpz_class r;
            mpz_fdiv_q(r.get_mpz_t(), vers_z(g).get_mpz_t(), dz.get_mpz_t());
            return Valeur::de_z(r);
        }
        if (op == "%") {
            mpz_class dz = vers_z(d);
            if (mpz_sgn(dz.get_mpz_t()) == 0) erreur_runtime("modulo par zéro");
            mpz_class r;
            mpz_fdiv_r(r.get_mpz_t(), vers_z(g).get_mpz_t(), dz.get_mpz_t());
            return Valeur::de_z(r);
        }
        if (op == "^") {
            if (d.ty == T_FLOTTANT) return Valeur::de_f(std::pow(vers_d(g), d.f));
            if (g.ty == T_FLOTTANT) return Valeur::de_f(std::pow(g.f, vers_d(d)));
            mpz_class expo = vers_z(d);
            if (!mpz_fits_slong_p(expo.get_mpz_t()))
                erreur_runtime("exposant démesuré (la puissance exploserait au-delà de tout calcul)");
            long e2 = mpz_get_si(expo.get_mpz_t());
            if (g.ty == T_RATIONNEL) {
                mpq_class r; rose_pow_rationnel(r, vers_q(g), e2); return Valeur::de_q(r);
            }
            mpz_class base = vers_z(g);
            if (e2 >= 0) {
                mpz_class r;
                mpz_pow_ui(r.get_mpz_t(), base.get_mpz_t(), (unsigned long)e2);
                return Valeur::de_z(r);
            }
            if (mpz_sgn(base.get_mpz_t()) == 0) erreur_runtime("0^exposant négatif");
            mpq_class r;
            rose_pow_rationnel(r, mpq_class(base), e2);
            return Valeur::de_q(r);
        }
        erreur_runtime("opérateur '" + op + "' inconnu");
        return Valeur::vide();
    }

    static bool numerique(const Valeur &v) {
        return v.ty == T_ENTIER || v.ty == T_RATIONNEL || v.ty == T_FLOTTANT || v.ty == T_BOOLEEN;
    }
    static bool valeur_egale(const Valeur &a, const Valeur &b) {
        if (a.ty == T_LISTE || b.ty == T_LISTE) {
            if (a.ty != T_LISTE || b.ty != T_LISTE || a.l->size() != b.l->size()) return false;
            for (size_t i = 0; i < a.l->size(); i++)
                if (!rose_valeur_egale((*a.l)[i], (*b.l)[i])) return false;
            return true;
        }
        if (a.ty == T_TABLE || b.ty == T_TABLE) {
            if (a.ty != T_TABLE || b.ty != T_TABLE || a.t->size() != b.t->size()) return false;
            for (const auto &kv : *a.t) {
                bool found = false;
                for (const auto &kv2 : *b.t)
                    if (rose_valeur_egale(kv.first, kv2.first) && rose_valeur_egale(kv.second, kv2.second)) { found = true; break; }
                if (!found) return false;
            }
            return true;
        }
        if (a.ty == T_CHAINE || b.ty == T_CHAINE)
            return a.ty == T_CHAINE && b.ty == T_CHAINE && a.s == b.s;
        if (a.ty == T_BOOLEEN || b.ty == T_BOOLEEN) {
            if (a.ty == T_BOOLEEN && b.ty == T_BOOLEEN) return a.b == b.b;
            return false;
        }
        return cmp_vals(a, b) == 0;
    }

    /* ── appels de fonctions ───────────────────────────────────────── */
    Valeur appel(E &e) {
        if (e.module == "dynamique") {
            std::vector<Valeur> args;
            for (PE &a : e.args) args.push_back(eval(a));
            return dynamique(e.texte, args);
        }
        if (e.args.size() == 1
                && (e.texte == "entier" || e.texte == "rationnel" || e.texte == "flottant"
                    || e.texte == "chaine")) {
            /* conversions explicites */
            Valeur v = eval(e.args[0]);
            if (e.texte == "entier") {
                if (v.ty == T_CHAINE) { mpz_class r; chaine_vers_entier(r, v.s); return Valeur::de_z(r); }
                return convertir(std::move(v), T_ENTIER);
            }
            if (e.texte == "rationnel") return convertir(std::move(v), T_RATIONNEL);
            if (e.texte == "flottant") {
                if (v.ty == T_CHAINE) return Valeur::de_f(chaine_vers_flottant(v.s));
                return convertir(std::move(v), T_FLOTTANT);
            }
            return Valeur::de_s(chaine_de(v));
        }
        auto it = prog.signatures.find(e.texte);
        if (it != prog.signatures.end()) {
            std::vector<Valeur> args;
            for (PE &a : e.args) args.push_back(eval(a));
            return appeler(*it->second, std::move(args));
        }
        std::vector<Valeur> args;
        for (PE &a : e.args) args.push_back(eval(a));
        return stdlib(e.texte, args, e.ligne);
    }

    /* ── fonctions dynamiques (dlopen à l'exécution) ──────────────── */
    Valeur dynamique(const std::string &nom, std::vector<Valeur> &args) {
        if (nom == "charger") {
            void *h = dlopen(args[0].s.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) erreur_runtime("chargement de bibliothèque impossible (" + args[0].s + ")");
            poignees_dyn.push_back(h);
            return Valeur::de_z(mpz_class((unsigned long)(uintptr_t)h));
        }
        if (nom == "fermer") {
            void *h = (void*)(uintptr_t)mpz_get_ui(args[0].z.get_mpz_t());
            dlclose(h);
            for (auto it = poignees_dyn.begin(); it != poignees_dyn.end(); ++it)
                if (*it == h) { poignees_dyn.erase(it); break; }
            return Valeur::vide();
        }
        if (nom == "appeler0" || nom == "appeler1" || nom == "appeler2") {
            void *h = (void*)(uintptr_t)mpz_get_ui(args[0].z.get_mpz_t());
            void *s = dlsym(h, args[1].s.c_str());
            if (!s) erreur_runtime("symbole introuvable dans la bibliothèque (" + args[1].s + ")");
            if (nom == "appeler0") {
                double (*fp)(void) = (double (*)(void))s;
                return Valeur::de_f(fp());
            }
            if (nom == "appeler1") {
                double a = vers_d(args[2]);
                if (args[2].ty == T_ENTIER) {
                    double (*fp)(long long) = (double (*)(long long))s;
                    return Valeur::de_f(fp(mpz_get_si(args[2].z.get_mpz_t())));
                }
                double (*fp)(double) = (double (*)(double))s;
                return Valeur::de_f(fp(a));
            }
            /* appeler2 : signatures doubles, mixtes */
            bool a_ent = args[2].ty == T_ENTIER, b_ent = args[3].ty == T_ENTIER;
            if (!a_ent && !b_ent) { auto fp = (double (*)(double, double))s; return Valeur::de_f(fp(vers_d(args[2]), vers_d(args[3]))); }
            if (a_ent && !b_ent)  { auto fp = (double (*)(long long, double))s; return Valeur::de_f(fp(mpz_get_si(vers_z(args[2]).get_mpz_t()), vers_d(args[3]))); }
            if (!a_ent && b_ent)  { auto fp = (double (*)(double, long long))s; return Valeur::de_f(fp(vers_d(args[2]), mpz_get_si(vers_z(args[3]).get_mpz_t()))); }
            { auto fp = (double (*)(long long, long long))s; return Valeur::de_f(fp(mpz_get_si(vers_z(args[2]).get_mpz_t()), mpz_get_si(vers_z(args[3]).get_mpz_t()))); }
        }
        erreur_runtime("fonction dynamique::" + nom + " inconnue");
        return Valeur::vide();
    }

    /* ── bibliothèque standard ─────────────────────────────────────── */


    void executer() {
        for (PI &i : prog.instructions) exec(i);
        std::fflush(stdout);
    }
};

/* Fins de ligne : un fichier édité sous Windows arrive en \r\n ; on
   normalise en \n à la lecture pour que le lexeur voie une seule ligne
   par ligne, sur tous les systèmes. */
static void normaliser_fins_de_ligne(std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < s.size() && s[i + 1] == '\n') i++;
        } else {
            out.push_back(s[i]);
        }
    }
    s.swap(out);
}

/* ── mode bundle : programme collé dans le moteur ─────────────────── */
static const char ROSE_MAGIC[8] = {'R','O','S','E','B','1','\x01','\x02'};

static std::string chemin_moteur() {
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(NULL, buf, 4096);
    if (n == 0 || n >= 4096) return {};
    int m = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return {};
    std::string sortie((size_t)m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, &sortie[0], m, nullptr, nullptr);
    return sortie;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return std::string(buf);
#endif
}

static bool lire_bundle(std::string &source) {
    std::string chemin = chemin_moteur();
    if (chemin.empty()) return false;
    std::ifstream in(ROSE_CHEMIN(chemin), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    long long taille = (long long)in.tellg();
    if (taille < 24) return false;
    in.seekg(taille - 16);
    unsigned char queue[16];
    in.read((char *)queue, 16);
    if (memcmp(queue + 8, ROSE_MAGIC, 8) != 0) return false;
    unsigned long long ln = 0;
    for (int i = 0; i < 8; i++) ln |= (unsigned long long)queue[i] << (8 * i);
    if (ln == 0 || (long long)ln > taille - 16) return false;
    in.seekg(taille - 16 - (long long)ln);
    source.resize((size_t)ln);
    in.read(&source[0], (std::streamsize)ln);
    if (!in) return false;
    normaliser_fins_de_ligne(source);
    return true;
}

static int comp_programme(const std::string &fichier, const std::string &sortie) {
    std::ifstream src(ROSE_CHEMIN(fichier), std::ios::binary);
    if (!src) { fprintf(stderr, "rose: fichier introuvable : %s\n", fichier.c_str()); return 1; }
    std::string source((std::stringstream() << src.rdbuf()).str());
    /* valide avant d'embarquer */
    Programme prog = analyser(source);
    replier_constants(prog);
    Verificateur verif(prog);
    verif.verifier();
    std::ifstream moteur(ROSE_CHEMIN(chemin_moteur()), std::ios::binary);
    if (!moteur) { fprintf(stderr, "rose: moteur illisible\n"); return 1; }
    std::ofstream out(ROSE_CHEMIN(sortie), std::ios::binary | std::ios::trunc);
    if (!out) { fprintf(stderr, "rose: ne peut écrire %s\n", sortie.c_str()); return 1; }
    out << moteur.rdbuf();
    out.write(source.data(), (std::streamsize)source.size());
    unsigned char queue[16];
    unsigned long long ln = source.size();
    for (int i = 0; i < 8; i++) queue[i] = (unsigned char)((ln >> (8 * i)) & 0xFF);
    memcpy(queue + 8, ROSE_MAGIC, 8);
    out.write((const char *)queue, 16);
    out.close();
    chmod(sortie.c_str(), 0755);
    return 0;
}

static int lire_fichier(const std::string &chemin, std::string &dest) {
    std::ifstream in(ROSE_CHEMIN(chemin), std::ios::binary);
    if (!in) return 1;
    std::stringstream ss; ss << in.rdbuf();
    dest = ss.str();
    normaliser_fins_de_ligne(dest);
    return 0;
}

struct ExecContexte {
    std::string *source;
    int code;
    std::string message;
    bool quitter;
};

/* ── Interface JIT natif (définie dans jit.cpp, compilation séparée) ──
   linkage C explicite pour correspondre à jit.cpp.                        */
extern "C" {
void*   rose_jit_compile(void* mod, void** helpers, int nhelpers);
Valeur* rose_jit_run(void* entry, void** helpers, int nhelpers, Valeur** frame);
}

/* Helpers d'opérations RIR (linkage C++, même mangling que jit.cpp)
   INDEXES — ordre contractuel identique à jit.h / jit.cpp */
extern Valeur* jit_val_z(int64_t v);        /* 0 */
extern Valeur* jit_val_b(bool v);           /* 1 */
extern Valeur* jit_add_z(Valeur* a, Valeur* b);   /* 2 */
extern Valeur* jit_sub_z(Valeur* a, Valeur* b);   /* 3 */
extern Valeur* jit_mul_z(Valeur* a, Valeur* b);   /* 4 */
extern int     jit_cmp_lt_z(Valeur* a, Valeur* b);/* 5 */
extern int     jit_test_b(Valeur* v);       /* 6 */
extern void    jit_afficher_z(Valeur* v);   /* 7 */
extern Valeur* jit_val_z_txt(const char* txt);    /* 8 */
extern Valeur* jit_div_z(Valeur* a, Valeur* b);   /* 9 */
extern Valeur* jit_rem_z(Valeur* a, Valeur* b);   /* 10 */
extern int     jit_cmp_eq_z(Valeur* a, Valeur* b);/* 11 */
extern int     jit_cmp_ne_z(Valeur* a, Valeur* b);/* 12 */
extern int     jit_cmp_le_z(Valeur* a, Valeur* b);/* 13 */
extern int     jit_cmp_gt_z(Valeur* a, Valeur* b);/* 14 */
extern int     jit_cmp_ge_z(Valeur* a, Valeur* b);/* 15 */
extern void    jit_afficher_b(Valeur* v);   /* 16 */
extern void    jit_afficher_s(Valeur* v);   /* 17 */
extern Valeur* jit_and_b(Valeur* a, Valeur* b);   /* 18 */
extern Valeur* jit_or_b(Valeur* a, Valeur* b);    /* 19 */
extern Valeur* jit_not_b(Valeur* a);        /* 20 */
extern Valeur* jit_neg_z(Valeur* a);        /* 21 */
extern Valeur* jit_add_z_into(Valeur* dst, Valeur* a, Valeur* b); /* 22 */
extern Valeur* jit_sub_z_into(Valeur* dst, Valeur* a, Valeur* b); /* 23 */
extern Valeur* jit_mul_z_into(Valeur* dst, Valeur* a, Valeur* b); /* 24 */
extern Valeur* jit_z_copy_into(Valeur* dst, Valeur* src);        /* 25 */
extern Valeur* jit_b_copy_into(Valeur* dst, Valeur* src);        /* 26 */
extern Valeur* jit_div_z_into(Valeur* dst, Valeur* a, Valeur* b);/* 27 */
extern Valeur* jit_rem_z_into(Valeur* dst, Valeur* a, Valeur* b);/* 28 */
extern Valeur* jit_neg_z_into(Valeur* dst, Valeur* a);           /* 29 */
extern Valeur* jit_bool_into(Valeur* dst, int cond);             /* 30 */

/* Nombre de helpers enregistrés pour le JIT */
static const int NJIT_HELPERS = 58;

/* v3.13.1 : les ponts JIT (builtins, index, afficher) lisent des Valeur*
   venues de slots JIT. Un slot de variable jamais affectée pré-porte un
   objet « vide » nommé (jit.cpp) — la lecture lève exactement l'erreur du
   cœur RIR, en direct : aucune trame asm à désenfiler. */
extern "C" const char* rose_jit_slot_name(const Valeur*);
static void verifier_lecture_jit(const Valeur* v) {
    if (!v || v->ty == T_VIDE) {
        fprintf(stderr, "Rose : erreur — variable '%s' utilisée sans valeur\n",
                rose_jit_slot_name(v));
        std::fflush(stdout);
        std::exit(1);
    }
}

/* Pont JIT → stdlib (M2.13) : les builtins numériques sont appelés depuis le
   code JIT natif par CE exact enregistrement — mêmes fonctions, mêmes
   sémantiques, mêmes erreurs que l'interpréteur. Enregistré en helper 50. */
static Valeur* rose_appel_builtin(const char* nom, Valeur** args, int nargs) {
    /* v3.13.1 : lecture d'une variable sans valeur depuis du code JIT — la
       même erreur que le cœur RIR, en direct (pas de désenfilage possible à
       travers les trames asm). */
    for (int j = 0; j < nargs; j++) verifier_lecture_jit(args[j]);
    // ── Chemin rapide machine (M2.16) : les fonctions dont le résultat exact
    // se calcule en int64 sont évaluées en natif, sans recopie profonde des
    // arguments ni distribution stdlib. Tout le reste — grands entiers, autres
    // types, débordement — retombe sur le chemin générique en dessous : le
    // résultat est le même, seul le chemin change.
    {
        long a0 = 0, a1 = 0; bool p0 = false, p1 = false;
        if (nargs >= 1 && args[0]->ty == T_ENTIER && mpz_fits_slong_p(args[0]->z.get_mpz_t()))
            { a0 = mpz_get_si(args[0]->z.get_mpz_t()); p0 = true; }
        if (nargs >= 2 && args[1]->ty == T_ENTIER && mpz_fits_slong_p(args[1]->z.get_mpz_t()))
            { a1 = mpz_get_si(args[1]->z.get_mpz_t()); p1 = true; }
        long r = 0; bool ok = false;
        auto ugcd = [](unsigned long u, unsigned long v) {
            while (v) { unsigned long t = u % v; u = v; v = t; } return u; };
        if (nargs == 1 && p0) {
            if (!std::strcmp(nom, "abs")) {
                if (a0 != LLONG_MIN) { r = a0 < 0 ? -a0 : a0; ok = true; }
            } else if (!std::strcmp(nom, "factorielle") && a0 >= 0 && a0 <= 20) {
                long f = 1; for (long i = 2; i <= a0; i++) f *= i;
                r = f; ok = true;
            } else if (!std::strcmp(nom, "fibonacci") && a0 >= 0 && a0 <= 92) {
                long u = 0, v = 1; bool over = false;
                for (long i = 0; i < a0; i++) {
                    long t; if (__builtin_add_overflow(u, v, &t)) { over = true; break; }
                    u = v; v = t;
                }
                if (!over) { r = u; ok = true; }
            }
        } else if (nargs == 2 && p0 && p1) {
            if (!std::strcmp(nom, "min")) { r = a0 <= a1 ? a0 : a1; ok = true; }
            else if (!std::strcmp(nom, "max")) { r = a0 >= a1 ? a0 : a1; ok = true; }
            else if ((!std::strcmp(nom, "pgcd") || !std::strcmp(nom, "ppcm")) &&
                     a0 != LLONG_MIN && a1 != LLONG_MIN) {
                unsigned long u = a0 < 0 ? (unsigned long)-(unsigned long)a0 : (unsigned long)a0;
                unsigned long v = a1 < 0 ? (unsigned long)-(unsigned long)a1 : (unsigned long)a1;
                if (!std::strcmp(nom, "pgcd")) { r = (long)ugcd(u, v); ok = true; }
                else {   // ppcm : encore une multiplication à vérifier
                    unsigned long g = ugcd(u, v);
                    unsigned long l;
                    if (!g) { r = 0; ok = true; }
                    else if (!__builtin_umull_overflow(u / g, v, &l) &&
                             l <= (unsigned long)LLONG_MAX) { r = (long)l; ok = true; }
                }
            } else if (!std::strcmp(nom, "puissance") && a1 >= 0) {
                long acc = 1, base = a0, e = a1; bool ovf = false;
                while (e > 0 && !ovf) {
                    if (e & 1) ovf = __builtin_mul_overflow(acc, base, &acc);
                    e >>= 1;
                    if (e > 0 && !ovf) ovf = __builtin_mul_overflow(base, base, &base);
                }
                if (!ovf) { r = acc; ok = true; }
            }
        }
        if (ok) return new Valeur(Valeur::de_z(mpz_class(r)));
    }
    std::vector<Valeur> va;
    for (int i = 0; i < nargs; i++) va.push_back(*args[i]);
    Valeur res = stdlib(std::string(nom), va, 0);
    return new Valeur(std::move(res));
}

/* Pont JIT → op « index » (M2.14) : copie fidèle du handler de
   l'interpréteur RIR (table, liste, chaîne). Helper 56. */
/* Pont JIT → afficher générique (M2.14) : types non suivus statiquement
   (liste, table, vide, range…). Miroir exact de l'op afficher RIR. */
static void rose_afficher_jit(Valeur* v) { verifier_lecture_jit(v); afficher(*v); }

static Valeur* rose_index(Valeur* base, Valeur* cle) {
    verifier_lecture_jit(base);
    verifier_lecture_jit(cle);
    if (base->ty == T_TABLE) {
        for (const auto &kv : *base->t)
            if (rose_valeur_egale(kv.first, *cle)) return new Valeur(kv.second);
        erreur_runtime("clé absente de la table");
    }
    if (base->ty == T_LISTE) {
        mpz_class idx = vers_z(*cle);
        size_t taille = base->l->size();
        if (!mpz_fits_ulong_p(idx.get_mpz_t()) || mpz_cmp_ui(idx.get_mpz_t(), taille) >= 0)
            erreur_runtime("index hors des limites de la liste");
        return new Valeur((*base->l)[(size_t)mpz_get_ui(idx.get_mpz_t())]);
    }
    if (base->ty == T_CHAINE) {
        mpz_class idx = vers_z(*cle);
        size_t taille = utf8_len(base->s);
        if (!mpz_fits_ulong_p(idx.get_mpz_t()) || mpz_cmp_ui(idx.get_mpz_t(), taille) >= 0)
            erreur_runtime("index hors des limites de la chaîne");
        return new Valeur(Valeur::de_s(utf8_char_str(base->s, (size_t)mpz_get_ui(idx.get_mpz_t()))));
    }
    erreur_runtime("index sur une valeur qui n'est pas une liste");
    return nullptr;
}

/* Le pipeline complet tourne sur un thread à très grande pile : la
   récursion Rose (éval → appel → exécution…) y est limitée par la
   mémoire, pas par la pile système de 8 Mo. */
static void *fil_pipeline(void *p) {
    ExecContexte *ctx = (ExecContexte*)p;
    try {
        Programme prog = analyser(*ctx->source);
        replier_constants(prog);
        Verificateur verif(prog);
        verif.verifier();
        /* Émission du module RIR */
        RirModule mod;
        EmetteurRir em(mod);
        for (auto &kv : prog.signatures) if (kv.second->corps) em.emettre_fonction(*kv.second);
        em.emettre_principal(prog);

        /* Trace RIR (activée par ROSE_RIR_DUMP=1) : blocs et instructions. */
        if (getenv("ROSE_RIR_DUMP")) {
            for (auto &f : mod.fonctions) {
                printf("fonction %s (params %zu)\n", f.nom.c_str(), f.params.size());
                for (auto &b : f.blocs) {
                    printf("  bloc %s\n", b.nom.c_str());
                    for (auto &in : b.instrs) {
                        printf("    %s dest=%s msg=%s cible=%s cible2=%s args=[", in.op.c_str(),
                               in.dest.c_str(), in.message.c_str(),
                               in.label_cible.c_str(), in.label_cible2.c_str());
                        for (auto &a : in.args) printf(" %s", a.name.c_str());
                        printf("]\n");
                    }
                }
            }
        }

        /* ── M2: essayer le JIT natif, sinon repli sur l'interpréteur ──
           ROSE_NO_JIT=1 force le repli (comparaisons de parité).        */
        static const bool no_jit = getenv("ROSE_NO_JIT") != nullptr;
        void* jit_helpers[NJIT_HELPERS] = {
            (void*)&jit_val_z,          /* 0  */
            (void*)&jit_val_b,          /* 1  */
            (void*)&jit_add_z,          /* 2  */
            (void*)&jit_sub_z,          /* 3  */
            (void*)&jit_mul_z,          /* 4  */
            (void*)&jit_cmp_lt_z,       /* 5  */
            (void*)&jit_test_b,         /* 6  */
            (void*)&jit_afficher_z,     /* 7  */
            (void*)&jit_val_z_txt,      /* 8  */
            (void*)&jit_div_z,          /* 9  */
            (void*)&jit_rem_z,          /* 10 */
            (void*)&jit_cmp_eq_z,       /* 11 */
            (void*)&jit_cmp_ne_z,       /* 12 */
            (void*)&jit_cmp_le_z,       /* 13 */
            (void*)&jit_cmp_gt_z,       /* 14 */
            (void*)&jit_cmp_ge_z,       /* 15 */
            (void*)&jit_afficher_b,     /* 16 */
            (void*)&jit_afficher_s,     /* 17 */
            (void*)&jit_and_b,          /* 18 */
            (void*)&jit_or_b,           /* 19 */
            (void*)&jit_not_b,          /* 20 */
            (void*)&jit_neg_z,          /* 21 */
            (void*)&jit_add_z_into,     /* 22 */
            (void*)&jit_sub_z_into,     /* 23 */
            (void*)&jit_mul_z_into,     /* 24 */
            (void*)&jit_z_copy_into,    /* 25 */
            (void*)&jit_b_copy_into,    /* 26 */
            (void*)&jit_div_z_into,     /* 27 */
            (void*)&jit_rem_z_into,     /* 28 */
            (void*)&jit_neg_z_into,     /* 29 */
            (void*)&jit_bool_into,      /* 30 */
        };
        jit_helpers[50] = (void*)&rose_appel_builtin;
        jit_helpers[56] = (void*)&rose_index;
        jit_helpers[57] = (void*)&rose_afficher_jit;
        void* jit_entry = no_jit ? nullptr
                                 : rose_jit_compile(&mod, jit_helpers, NJIT_HELPERS);

        if (jit_entry) {
            /* Exécution JIT */
            const int frame_size = 256;
            Valeur** frame = new Valeur*[frame_size];
            for (int i = 0; i < frame_size; i++) frame[i] = nullptr;
            Valeur* result = rose_jit_run(jit_entry, jit_helpers, NJIT_HELPERS, frame);
            /* Libération : le JIT peut partager un même Valeur* entre plusieurs
               slots (constantes matérialisées, alias de variables) → dédupliquer. */
            std::set<void*> deja_supprimes;
            if (result) deja_supprimes.insert(result);
            for (int i = 0; i < frame_size; i++)
                if (frame[i] && deja_supprimes.insert(frame[i]).second) delete frame[i];
            if (result) delete result;
            delete[] frame;
            ctx->code = 0;
        } else {
            /* Repli : interpréteur RIR (cœur du moteur M1) */
            InterpRir interp(mod);
        interp.prog = &prog;
            interp.executer();
            ctx->code = 0;
        }
    } catch (const Quitter &q) {
        ctx->code = q.code;
        ctx->quitter = true;                      /* pas de message d'erreur */
    } catch (const ErreurRose &e) {
        ctx->code = 1;
        ctx->message = e.texte();
    } catch (const std::exception &e) {
        ctx->code = 1;
        ctx->message = e.what();
    }
    return nullptr;
}

#ifdef _WIN32
static unsigned __stdcall entree_pipeline(void *p) {
    fil_pipeline(p);
    return 0;
}
#endif

static int executer_source(const std::string &source) {
    ExecContexte ctx{const_cast<std::string*>(&source), 0, {}, false};
#ifdef _WIN32
    unsigned ident = 0;
    uintptr_t t = _beginthreadex(nullptr, (unsigned)((size_t)512 * 1024 * 1024),
                                 entree_pipeline, &ctx, 0, &ident);
    if (t) {
        WaitForSingleObject((HANDLE)t, INFINITE);
        CloseHandle((HANDLE)t);
    } else {
        fil_pipeline(&ctx);                   /* repli : en ligne */
    }
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t taille = (size_t)512 * 1024 * 1024;
    if (pthread_attr_setstacksize(&attr, taille) != 0)
        pthread_attr_init(&attr);            /* repli : taille par défaut */
    pthread_t fil;
    int cre = pthread_create(&fil, &attr, fil_pipeline, &ctx);
    pthread_attr_destroy(&attr);
    if (cre != 0) {                           /* plus de thread possible : en ligne */
        fil_pipeline(&ctx);
    } else {
        pthread_join(fil, nullptr);
    }
#endif
    if (ctx.code && !ctx.quitter) {
        std::fflush(stdout);
        fprintf(stderr, "Rose : erreur — %s\n", ctx.message.c_str());
    }
    return ctx.code;
}

static const char *USAGE =
    "Rose 2.0 — moteur natif (binaire autonome, maths exactes embarquées)\n"
    "Usage :\n"
    "  rose prog.rose            interpréter un programme Rose\n"
    "  rose run prog.rose        interpréter (identique)\n"
    "  rose build prog.rose [-o sortie] [--llvm]  compiler natif (C++ ou LLVM IR)\n"
    "  rose comp prog.rose [-o sortie]   produire un exécutable autonome\n"
    "  rose rir prog.rose             dumper la représentation intermédiaire (RIR)\n"
    "  rose info                 état du moteur et des dépendances\n"
    "  rose version              numéro de version\n";

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Console UTF-8 : messages accentués et notation périodique. */
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    std::string source;
    if (lire_bundle(source)) {
        for (int i = 1; i < argc; i++) ARGUMENTS_PROG.push_back(argv[i]);
        return executer_source(source);
    }

    std::string action;
    if (argc >= 2) action = argv[1];
    if (action == "version") { printf("Rose 3.14.0 — moteur natif C++/GMP\n"); return 0; }
    if (action == "info") {
        printf("rose 3.14.0\n");
#ifdef _WIN32
        printf("  moteur            : %s — binaire autonome, seule dépendance : UCRT (Windows)\n",
               sizeof(void*) == 8 ? "natif 64 bits" : "natif");
#else
        printf("  moteur            : %s — binaire autonome, aucune dépendance hors libc\n",
               sizeof(void*) == 8 ? "natif 64 bits" : "natif");
#endif
        printf("  maths exactes     : GMP liée statiquement (entiers et rationnels sans limite)\n");
        printf("  interpréteur      : intégré (mode par défaut)\n");
        printf("  compilation       : rose comp — exécutable autonome (moteur + programme)\n");
#ifdef _WIN32
        printf("  FFI               : @dll/@sym + dynamique:: via LoadLibrary (msvcrt intégrée)\n");
#else
        printf("  FFI               : @dll/@sym + dynamique:: via dlopen (libc/libm intégrés)\n");
#endif
        return 0;
    }
    if (action == "aide" || action == "--help" || action == "-h") { fputs(USAGE, stdout); return 0; }
    if (action.empty()) { fputs(USAGE, stderr); return 1; }

    std::string fichier;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--cible") || action == "export") {
            fprintf(stderr,
                    "rose: le moteur natif interprète (rose prog.rose) et produit des\n"
                    "      exécutables autonomes (rose comp prog.rose) sans cible externe.\n"
                    "      Pour émettre du code C/Python/Julia/Rust/Zig : rose2/bin/rose\n"
                    "      run fichier.rose --cible X\n");
            return 1;
        }
    if (action == "run") {
        int pos = 0;
        for (int i = 2; i < argc; i++)
            if (argv[i][0] != '-') { fichier = argv[i]; pos = i; break; }
        if (fichier.empty()) { fprintf(stderr, "rose: quel programme interpréter ?\n"); return 1; }
        for (int i = pos + 1; i < argc; i++) ARGUMENTS_PROG.push_back(argv[i]);
        if (lire_fichier(fichier, source)) { fprintf(stderr, "rose: fichier introuvable : %s\n", fichier.c_str()); return 1; }
        return executer_source(source);
    }
    if (action == "rir-run") {
        if (argc < 3) { fprintf(stderr, "rose: quel programme ?\n"); return 1; }
        std::string source;
        if (lire_fichier(argv[2], source)) { fprintf(stderr, "rose: fichier introuvable : %s\n", argv[2]); return 1; }
        for (int i = 3; i < argc; i++) ARGUMENTS_PROG.push_back(argv[i]);
        Programme prog = analyser(source);
        replier_constants(prog);
        Verificateur verif(prog);
        verif.verifier();
        RirModule mod;
        EmetteurRir em(mod);
        for (auto &kv : prog.signatures) if (kv.second->corps) em.emettre_fonction(*kv.second);
        em.emettre_principal(prog);
        InterpRir interp(mod);
        interp.prog = &prog;
        interp.executer();
        return 0;
    }
    if (action == "verif") {
        if (argc < 3) { fprintf(stderr, "rose: quel programme ?\n"); return 1; }
        std::string source;
        if (lire_fichier(argv[2], source)) { fprintf(stderr, "rose: fichier introuvable : %s\n", argv[2]); return 1; }
        try {
            Programme prog = analyser(source);
            replier_constants(prog);
            Verificateur verif(prog);
            verif.verifier();
        } catch (const ErreurRose &e) {
            fprintf(stderr, "Rose : erreur — %s\n", e.texte().c_str());
            return 1;
        }
        fputs("vérifié\n", stdout);
        return 0;
    }
    if (action == "ast") {
        if (argc < 3) { fprintf(stderr, "rose: quel programme ?\n"); return 1; }
        std::string source;
        if (lire_fichier(argv[2], source)) { fprintf(stderr, "rose: fichier introuvable : %s\n", argv[2]); return 1; }
        try {
            fputs(ast_programme(analyser(source)).c_str(), stdout);
        } catch (const ErreurRose &e) {
            fprintf(stderr, "Rose : erreur — %s\n", e.texte().c_str());
            return 1;
        }
        return 0;
    }
    if (action == "tokens") {
        if (argc < 3) { fprintf(stderr, "rose: quel programme ?\n"); return 1; }
        std::string source;
        if (lire_fichier(argv[2], source)) { fprintf(stderr, "rose: fichier introuvable : %s\n", argv[2]); return 1; }
        try {
            for (const Token &t : tokeniser(source))
                printf("%d:%d %s %s\n", t.ligne, t.colonne, tk_nom(t.type), texte_echappe(t.texte).c_str());
        } catch (const ErreurRose &e) {
            fprintf(stderr, "Rose : erreur — %s\n", e.texte().c_str());
            return 1;
        }
        return 0;
    }
    if (action == "rir") {
        if (argc < 3) { fprintf(stderr, "rose: quel programme ?\n"); return 1; }
        std::string source;
        if (lire_fichier(argv[2], source)) { fprintf(stderr, "rose: fichier introuvable : %s\n", argv[2]); return 1; }
        try {
            Programme prog = analyser(source);
            replier_constants(prog);
            Verificateur verif(prog);
            verif.verifier();
            std::string rir = emettre_rir(prog);
            fputs(rir.c_str(), stdout);
        } catch (const ErreurRose &e) {
            fprintf(stderr, "Rose : erreur — %s\n", e.texte().c_str());
            return 1;
        }
        return 0;
    }
    if (action == "comp") {
        std::string sortie;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--sortie")) {
                if (i + 1 < argc) sortie = argv[++i];
            } else if (argv[i][0] != '-') fichier = argv[i];
        }
        if (fichier.empty()) { fprintf(stderr, "rose: quel programme compiler ?\n"); return 1; }
        if (sortie.empty()) {
#ifdef _WIN32
            size_t slash = fichier.find_last_of("\\/");
#else
            size_t slash = fichier.find_last_of('/');
#endif
            sortie = fichier.substr(slash + 1);
            size_t pt = sortie.rfind(".rose");
            if (pt != std::string::npos) sortie.resize(pt);
            if (sortie.empty()) sortie = "programme";
        }
#ifdef _WIN32
        if (sortie.size() < 4 ||
            tolower((unsigned char)sortie[sortie.size() - 4]) != '.' ||
            tolower((unsigned char)sortie[sortie.size() - 3]) != 'e' ||
            tolower((unsigned char)sortie[sortie.size() - 2]) != 'x' ||
            tolower((unsigned char)sortie[sortie.size() - 1]) != 'e')
            sortie += ".exe";
#endif
        int r;
        try {
            r = comp_programme(fichier, sortie);
        } catch (const ErreurRose &e) {
            fprintf(stderr, "Rose : erreur — %s\n", e.texte().c_str());
            return 1;
        } catch (const std::exception &e) {
            fprintf(stderr, "Rose : erreur — %s\n", e.what());
            return 1;
        }
        if (r == 0) fprintf(stderr, "rose: %s produit (exécutable autonome)\n", sortie.c_str());
        return r;
    }
    if (action == "build") {
        std::string sortie;
        bool use_llvm = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--sortie")) {
                if (i + 1 < argc) sortie = argv[++i];
            } else if (!strcmp(argv[i], "--llvm") || !strcmp(argv[i], "-llvm")) {
                use_llvm = true;
            } else if (argv[i][0] != '-') fichier = argv[i];
        }
        if (fichier.empty()) { fprintf(stderr, "rose: quel programme compiler ?\n"); return 1; }
        if (sortie.empty()) {
            size_t slash = fichier.find_last_of("/\\");
            sortie = fichier.substr(slash + 1);
            size_t pt = sortie.rfind(".rose");
            if (pt != std::string::npos) sortie.resize(pt);
            if (sortie.empty()) sortie = "programme";
        }
        // Lire et analyser — le front-end doit rejeter avec le même message
        // que l'interprète (ErreurRose → « Rose : erreur — … », code 1).
        std::string source;
        if (lire_fichier(fichier, source)) { fprintf(stderr, "rose: fichier introuvable : %s\n", fichier.c_str()); return 1; }
        Programme prog;
        RirModule mod;
        try {
            prog = analyser(source);
            replier_constants(prog);
            Verificateur verif(prog);
            verif.verifier();
            EmetteurRir em(mod);
            for (auto &kv : prog.signatures) if (kv.second->corps) em.emettre_fonction(*kv.second);
            em.emettre_principal(prog);
        } catch (const ErreurRose &e) {
            fprintf(stderr, "Rose : erreur — %s\n", e.texte().c_str());
            return 1;
        } catch (const std::exception &e) {
            fprintf(stderr, "Rose : erreur — %s\n", e.what());
            return 1;
        }
        // Transpiler — le rejet explicite du transpileur (M4.2 : jamais de
        // trou silencieux) est attrapé ici au même format que le checker.
        std::string cpp_code;
        try {
            cpp_code = transpiler_rir(mod);
        } catch (const std::exception &e) {
            fprintf(stderr, "Rose : erreur — %s\n", e.what());
            return 1;
        }
        // Écrire le .cpp
        std::string cpp_file = sortie + ".cpp";
        std::ofstream out(cpp_file, std::ios::binary | std::ios::trunc);
        if (!out) { fprintf(stderr, "rose: ne peut écrire %s\n", cpp_file.c_str()); return 1; }
        out << cpp_code;
        out.close();
        // Compiler avec g++ — chemins portables : le runtime est cherché à côté
        // de l'exécutable (ROSE_HOME pour override) ; compilateur, drapeaux et
        // chemin GMP surchargables par ROSE_CXX / ROSE_CXXFLAGS / ROSE_GMP.
        std::string exe_rep = chemin_moteur();
        { size_t s = exe_rep.find_last_of("/\\");
          if (s != std::string::npos) exe_rep.resize(s); }
        const char *rh = getenv("ROSE_HOME");
        std::string runtime, entete_rep;
        {
            std::vector<std::string> cands;
            if (rh && *rh) cands.push_back(rh);
            if (!exe_rep.empty()) { cands.push_back(exe_rep + "/src"); cands.push_back(exe_rep);
                                    cands.push_back(exe_rep + "/../src"); }
            for (auto &b : cands) {
                std::ifstream t(b + "/rose_runtime.cpp");
                if (t.good()) { runtime = b + "/rose_runtime.cpp"; entete_rep = b; break; }
            }
        }
        if (runtime.empty()) {
            fprintf(stderr, "rose build : rose_runtime.cpp introuvable à côté de rose"
                            " (définir ROSE_HOME)\n");
            return 1;
        }
        const char *cxx = getenv("ROSE_CXX"); if (!cxx || !*cxx) cxx = "g++";
        const char *cfl = getenv("ROSE_CXXFLAGS");
        std::string flags = (cfl && *cfl) ? cfl : "-O3 -march=native";
        const char *gmp = getenv("ROSE_GMP");
        std::string gmp_flags = (gmp && *gmp) ? (" -I" + std::string(gmp) + "/include -L" + std::string(gmp) + "/lib") : "";
        std::string ll_file = sortie + ".ll";
        int r = 1;
        if (use_llvm) {
            const char *clangbin = getenv("ROSE_CLANG"); if (!clangbin || !*clangbin) clangbin = "clang++";
            std::string cmd_ll = std::string(clangbin) + " -S -emit-llvm " + flags + " -std=c++17" +
                                 " -I" + entete_rep + gmp_flags +
                                 " " + cpp_file + " -o " + ll_file + " 2>&1";
            fprintf(stderr, "rose llvm: %s\n", cmd_ll.c_str());
            r = system(cmd_ll.c_str());
            if (r != 0) { fprintf(stderr, "rose: emission LLVM IR échouée\n"); return 1; }
            std::string cmd = std::string(clangbin) + " " + flags + " -std=c++17" +
                              " " + ll_file + " " + runtime +
                              " -I" + entete_rep + gmp_flags +
                              " -o " + sortie + " -lgmp -lgmpxx -lm -lpthread 2>&1";
            fprintf(stderr, "rose llvm: %s\n", cmd.c_str());
            r = system(cmd.c_str());
            if (r == 0) fprintf(stderr, "rose: %s produit (LLVM IR : %s)\\n", sortie.c_str(), ll_file.c_str());
        } else {
            std::string cmd = std::string(cxx) + " " + flags + " -std=c++17" +
                              " -I" + entete_rep + gmp_flags +
                              " " + cpp_file + " " + runtime +
                              " -o " + sortie + " -lgmp -lgmpxx -lm -lpthread 2>&1";
            fprintf(stderr, "rose build: %s\n", cmd.c_str());
            r = system(cmd.c_str());
            if (r == 0) fprintf(stderr, "rose: %s produit (compilé natif)\\n", sortie.c_str());
        }
        return r == 0 ? 0 : 1;
    }
    /* premier argument = fichier .rose sans sous-commande */
    if (action.size() > 5 && action.compare(action.size() - 5, 5, ".rose") == 0) {
        for (int i = 2; i < argc; i++) ARGUMENTS_PROG.push_back(argv[i]);
        if (lire_fichier(action, source)) { fprintf(stderr, "rose: fichier introuvable : %s\n", action.c_str()); return 1; }
        return executer_source(source);
    }
    fprintf(stderr, "rose: commande inconnue '%s'\n\n", action.c_str());
    fputs(USAGE, stderr);
    return 1;
}
