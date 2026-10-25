#ifndef ROSE_RUNTIME_H
#define ROSE_RUNTIME_H

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <string>
#include <memory>
#include <vector>
#include <unistd.h>
#include <gmpxx.h>

// Types runtime de Rose (transpilation AOT).
typedef int64_t rose_int;

// ── rose_ent : entier machine avec promotion GMP à l'overflow (M3.4) ──
// Sémantique exacte de l'interprète : tout tient en int64 quand ça tient,
// et les opérations débordantes retombent sur GMP sans jamais WrapPer.
#include <sys/resource.h>
#include <unistd.h>

// Récursion profonde en code AOT : les cadres contiennent des rose_ent (plus
// lourds que l'ancien long long). Un programme récursif lève sa propre pile
// de 512 Mo et se relance une fois — invisible pour l'utilisateur (aucune
// sortie n'a encore eu lieu) ; jamais payé par les programmes non récursifs.
inline void rose_pile_geante(int argc, char **argv) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) != 0) return;
    const rlim_t cible = 512ull << 20;
    rlim_t viser = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max > cible) ? cible : rl.rlim_max;
    if (viser <= rl.rlim_cur) return;              // impossible ou déjà faite
    rl.rlim_cur = viser;
    if (setrlimit(RLIMIT_STACK, &rl) != 0) return; // droit insuffisant : tant pis
    extern char **environ;
    execv("/proc/self/exe", argv);
    // execv ne revient qu'en cas de succès ; sinon on tourne avec la pile d'avant.
    (void)argc; (void)environ;
}

class rose_ent;
inline double rose_vers_double(const rose_ent &e);

class rose_ent {
    /* Représentation 16 octets (2 registres de retour d'appel, pas de
       round-trip mémoire sret) : m_ porte la valeur tant que le nombre tient
       int64 ; g_ n'existe QUE pour les grands (convention NULL :
       gros() ≡ g_ != nullptr). La version précédente portait le mpz_class en
       dur (32 o par copie, dont 16 o de structure GMP morte pour les petits —
       mesuré : boucle_entier 0,11 s contre 0,05 en JIT, la copie sret est le
       differentiel). Décisions sémantiques inchangées : mêmes tests de
       promotion, mêmes domaines, mêmes messages. */
    int64_t m_;
    mpz_class *g_;   // null sauf si gros() — allouée à la promotion
public:
    rose_ent(int64_t v = 0) : m_(v), g_(nullptr) {}
    rose_ent(const mpz_class &z) : m_(0), g_(nullptr) {
        if (mpz_fits_slong_p(z.get_mpz_t())) m_ = mpz_get_si(z.get_mpz_t());
        else g_ = new mpz_class(z);
    }
    rose_ent(const rose_ent &o) : m_(o.m_), g_(o.g_ ? new mpz_class(*o.g_) : nullptr) {}
    rose_ent(rose_ent &&o) : m_(o.m_), g_(o.g_) { o.g_ = nullptr; }
    ~rose_ent() { delete g_; }
    rose_ent &operator=(const rose_ent &o) {
        if (this != &o) { delete g_; m_ = o.m_; g_ = o.g_ ? new mpz_class(*o.g_) : nullptr; }
        return *this;
    }
    rose_ent &operator=(rose_ent &&o) {
        if (this != &o) { delete g_; m_ = o.m_; g_ = o.g_; o.g_ = nullptr; }
        return *this;
    }
    // affectation des slots grands entiers : t_gd = rose_vers_mpz(t_ent).
    // EXPLICITE : la moindre conversion implicite vers mpz ferait entrer le
    // machinery de templates de gmpxx dans la resolution d'overload
    // (ambiguite « rose_gd = rose_ent ») voire pire.
    rose_ent &operator=(const mpz_class &z) {
        if (mpz_fits_slong_p(z.get_mpz_t())) { delete g_; g_ = nullptr; m_ = mpz_get_si(z.get_mpz_t()); }
        else { if (!g_) g_ = new mpz_class; *g_ = z; }
        return *this;
    }
    rose_ent &operator=(long long v) { delete g_; g_ = nullptr; m_ = (int64_t)v; return *this; }
    explicit operator mpz_class() const {
        if (g_) return *g_;
        mpz_class r; mpz_set_si(r.get_mpz_t(), (long)m_); return r;
    }
    // promotion implicite vers le domaine flottant (comme l'ancien long long :
    // « t_flottant = $entier » et les appels *_f acceptent désormais les ent).
    // Volontairement SANS constructeur rose_ent(double) : il rendrait
    // « ent + 5 » ambigu. Les retours F → slot ent passent par
    // operator=(long long) avec la troncature habituelle.
    explicit operator double() const { return rose_vers_double(*this); }
    bool gros() const { return g_ != nullptr; }
    int64_t petit() const { return m_; }       // valide si !gros()
    const mpz_class &gd() const { return *g_; } // valide si gros()
    std::string texte() const;
};
static_assert(sizeof(rose_ent) == 16,
              "rose_ent doit tenir dans 2 registres (levier perf M4.1)");


// conversion uniforme des trois domaines entiers vers mpz
inline double rose_vers_double(double v) { return v; }
// miroir exact de vers_d (rose.cpp) : Q→F = mpq_get_d, Z→GMP→F = mpz_get_d.
inline double rose_vers_double(const mpq_class &q) { return mpq_get_d(q.get_mpq_t()); }
inline double rose_vers_double(const mpz_class &z) { return mpz_get_d(z.get_mpz_t()); }
inline double rose_vers_double(const rose_ent &e) {
    return e.gros() ? mpz_get_d(e.gd().get_mpz_t()) : (double)e.petit();
}
inline mpz_class rose_vers_mpz(const rose_ent &e) {
    if (e.gros()) return e.gd();
    mpz_class r; mpz_set_si(r.get_mpz_t(), (long)e.petit()); return r;
}
inline mpz_class rose_vers_mpz(const mpz_class &z) { return z; }
inline mpz_class rose_vers_mpz(const mpq_class &q) {   // miroir vers_z : PLANCHER
    mpz_class r; mpz_fdiv_q(r.get_mpz_t(), q.get_num().get_mpz_t(), q.get_den().get_mpz_t());
    return r;
}
inline mpz_class rose_vers_mpz(long long v) {
    mpz_class r; mpz_set_si(r.get_mpz_t(), (long)v); return r;
}
inline mpq_class rose_q_de(const rose_ent &e) {
    return e.gros() ? mpq_class(e.gd()) : mpq_class(rose_vers_mpz(e.petit()));
}
inline mpq_class rose_q_de(const mpz_class &z) { return mpq_class(z); }
inline mpq_class rose_q_de(const mpq_class &q) { return q; }
inline mpq_class rose_q_de(double v) { return mpq_class(v); }   // binaire exact, comme l'ancien rose_qd(double)
inline mpq_class rose_q_de(long long v) { return mpq_class(rose_vers_mpz(v)); }

/* ── Q ⊛ I mélangés — v3.13.1 : quand le checker laisse passer une opération
   rationnel⊛entier, l'émis C++ brut doit promouvoir l'entier en rationnel,
   comme l'interprète. Trouvé par le testeur génératif (graine 19). ─────── */
inline mpq_class operator+(const rose_ent &a, const mpq_class &b) { return rose_q_de(a) + b; }
inline mpq_class operator+(const mpq_class &a, const rose_ent &b) { return a + rose_q_de(b); }
inline mpq_class operator-(const rose_ent &a, const mpq_class &b) { return rose_q_de(a) - b; }
inline mpq_class operator-(const mpq_class &a, const rose_ent &b) { return a - rose_q_de(b); }
inline mpq_class operator*(const rose_ent &a, const mpq_class &b) { return rose_q_de(a) * b; }
inline mpq_class operator*(const mpq_class &a, const rose_ent &b) { return a * rose_q_de(b); }
inline mpq_class operator/(const rose_ent &a, const mpq_class &b) { return rose_q_de(a) / b; }
inline mpq_class operator/(const mpq_class &a, const rose_ent &b) { return a / rose_q_de(b); }
inline mpq_class operator+(const mpz_class &a, const mpq_class &b) { return rose_q_de(a) + b; }
inline mpq_class operator+(const mpq_class &a, const mpz_class &b) { return a + rose_q_de(b); }
inline mpq_class operator-(const mpz_class &a, const mpq_class &b) { return rose_q_de(a) - b; }
inline mpq_class operator-(const mpq_class &a, const mpz_class &b) { return a - rose_q_de(b); }
inline mpq_class operator*(const mpz_class &a, const mpq_class &b) { return rose_q_de(a) * b; }
inline mpq_class operator*(const mpq_class &a, const mpz_class &b) { return a * rose_q_de(b); }
inline mpq_class operator/(const mpz_class &a, const mpq_class &b) { return rose_q_de(a) / b; }
inline mpq_class operator/(const mpq_class &a, const mpz_class &b) { return a / rose_q_de(b); }

// promotion d'un résultat : petit mot machine sinon GMP
inline rose_ent rose_promu(const mpz_class &z) { return rose_ent(z); }

// arithmétique : chemin machine vérifié, promotion sinon
inline rose_ent operator+(const rose_ent &a, const rose_ent &b) {
    if (!a.gros() && !b.gros()) {
        __int128 r = (__int128)a.petit() + b.petit();
        if (r >= INT64_MIN && r <= INT64_MAX) return rose_ent((int64_t)r);
    }
    return rose_promu(rose_vers_mpz(a) + rose_vers_mpz(b));
}
inline rose_ent operator-(const rose_ent &a, const rose_ent &b) {
    if (!a.gros() && !b.gros()) {
        __int128 r = (__int128)a.petit() - b.petit();
        if (r >= INT64_MIN && r <= INT64_MAX) return rose_ent((int64_t)r);
    }
    return rose_promu(rose_vers_mpz(a) - rose_vers_mpz(b));
}
inline rose_ent operator*(const rose_ent &a, const rose_ent &b) {
    if (!a.gros() && !b.gros()) {
        __int128 r = (__int128)a.petit() * b.petit();
        if (r >= INT64_MIN && r <= INT64_MAX) return rose_ent((int64_t)r);
    }
    return rose_promu(rose_vers_mpz(a) * rose_vers_mpz(b));
}
inline rose_ent operator-(const rose_ent &a) {
    if (!a.gros() && a.petit() != INT64_MIN) return rose_ent(-a.petit());
    return rose_promu(-rose_vers_mpz(a));
}
inline bool operator<(const rose_ent &a, const rose_ent &b) {
    if (!a.gros() && !b.gros()) return a.petit() < b.petit();
    return rose_vers_mpz(a) < rose_vers_mpz(b);
}
inline bool operator<=(const rose_ent &a, const rose_ent &b) { return !(b < a); }
inline bool operator>(const rose_ent &a, const rose_ent &b) { return b < a; }
inline bool operator>=(const rose_ent &a, const rose_ent &b) { return !(a < b); }
inline bool operator==(const rose_ent &a, const rose_ent &b) {
    if (a.gros() != b.gros()) return false;
    return a.gros() ? a.gd() == b.gd() : a.petit() == b.petit();
}
inline bool operator!=(const rose_ent &a, const rose_ent &b) { return !(a == b); }

// Flottant × entier : laissé aux opérateurs built-in — la conversion
// d'opérateur opérateur double() de rose_ent fait la promotion exactement
// comme l'ancien long long. Des surcharges (rose_ent, double) explicites
// créeraient une ambiguïté « rose_ent + int » (int→long long et int→double
// sont deux conversions de même rang).
// Littéraux et entiers machine : surcharges exactes — sans elles, « ent + 5 »
// hésiterait entre rose_ent(int64) et mpz(int) (deux conversions utilisateur).
inline rose_ent operator+(const rose_ent &a, long long b) { return a + rose_ent(b); }
inline rose_ent operator+(long long a, const rose_ent &b) { return rose_ent(a) + b; }
inline rose_ent operator-(const rose_ent &a, long long b) { return a - rose_ent(b); }
inline rose_ent operator-(long long a, const rose_ent &b) { return rose_ent(a) - b; }
inline rose_ent operator*(const rose_ent &a, long long b) { return a * rose_ent(b); }
inline rose_ent operator*(long long a, const rose_ent &b) { return rose_ent(a) * b; }
inline bool operator<(const rose_ent &a, long long b) { return a < rose_ent(b); }
inline bool operator<(long long a, const rose_ent &b) { return rose_ent(a) < b; }
inline bool operator>(const rose_ent &a, long long b) { return rose_ent(b) < a; }
inline bool operator>(long long a, const rose_ent &b) { return b < rose_ent(a); }
inline bool operator<=(const rose_ent &a, long long b) { return !(rose_ent(b) < a); }
inline bool operator<=(long long a, const rose_ent &b) { return !(b < rose_ent(a)); }
inline bool operator>=(const rose_ent &a, long long b) { return !(a < rose_ent(b)); }
inline bool operator>=(long long a, const rose_ent &b) { return !(rose_ent(a) < b); }
inline bool operator==(const rose_ent &a, long long b) { return a == rose_ent(b); }
inline bool operator==(long long a, const rose_ent &b) { return rose_ent(a) == b; }
inline bool operator!=(const rose_ent &a, long long b) { return !(a == rose_ent(b)); }
inline bool operator!=(long long a, const rose_ent &b) { return !(rose_ent(a) == b); }

// interop avec les slots grand entier (mpz) : promotion par le chemin GMP
inline mpz_class operator+(const rose_ent &a, const mpz_class &b) { return rose_vers_mpz(a) + b; }
inline mpz_class operator+(const mpz_class &a, const rose_ent &b) { return a + rose_vers_mpz(b); }
inline mpz_class operator-(const rose_ent &a, const mpz_class &b) { return rose_vers_mpz(a) - b; }
inline mpz_class operator-(const mpz_class &a, const rose_ent &b) { return a - rose_vers_mpz(b); }
inline mpz_class operator*(const rose_ent &a, const mpz_class &b) { return rose_vers_mpz(a) * b; }
inline mpz_class operator*(const mpz_class &a, const rose_ent &b) { return a * rose_vers_mpz(b); }
inline bool operator<(const rose_ent &a, const mpz_class &b) { return rose_vers_mpz(a) < b; }
inline bool operator<(const mpz_class &a, const rose_ent &b) { return a < rose_vers_mpz(b); }
inline bool operator>(const rose_ent &a, const mpz_class &b) { return b < a; }
inline bool operator>(const mpz_class &a, const rose_ent &b) { return a > rose_vers_mpz(b); }
inline bool operator<=(const rose_ent &a, const mpz_class &b) { return !(b < a); }
inline bool operator<=(const mpz_class &a, const rose_ent &b) { return !(a > rose_vers_mpz(b)); }
inline bool operator>=(const rose_ent &a, const mpz_class &b) { return !(a < b); }
inline bool operator>=(const mpz_class &a, const rose_ent &b) { return !(a < rose_vers_mpz(b)); }
inline bool operator==(const rose_ent &a, const mpz_class &b) { return rose_vers_mpz(a) == b; }
inline bool operator==(const mpz_class &a, const rose_ent &b) { return a == rose_vers_mpz(b); }
inline bool operator!=(const rose_ent &a, const mpz_class &b) { return !(a == b); }
inline bool operator!=(const mpz_class &a, const rose_ent &b) { return !(a == b); }

typedef double rose_float;
typedef bool rose_bool;
typedef std::string rose_str;
typedef std::shared_ptr<std::string> rose_tampon;
typedef mpz_class rose_gd;   // grand entier (précision arbitraire, GMP)

// Grands entiers (fidèles à l'interprète : mêmes algorithmes GMP).
void rose_afficher_gd(const rose_gd &n);

// Rationnels exacts (fidèles à l'interprète : écriture avec périodes).
typedef mpq_class rose_qd;
rose_qd rose_q_div(const rose_qd &a, const rose_qd &b);
int rose_q_cmp(const rose_qd &a, const rose_qd &b);
bool rose_approx_q(const rose_qd &a, const rose_qd &b, bool au_moins_un_flottant);
bool rose_approx_f(rose_float a, rose_float b);
void rose_afficher_q(const rose_qd &q);

// Divisions euclidiennes à la Rose (plancher, miroir de l'interprète).

// Générateur pseudo-aléatoire (splitmix64, état global partagé) — inlinable.
extern uint64_t g_rng_etat;

static inline uint64_t rose_rng_tirer() {
    uint64_t z = (g_rng_etat += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline void rose_graine(rose_int seed) {
    g_rng_etat = (uint64_t)seed;
    rose_rng_tirer();
}

// Tampons d'octets.
rose_tampon rose_tampon_new(size_t n);
rose_tampon rose_octets(const char *s);
rose_int rose_tampon_longueur(rose_tampon buf);
rose_int rose_tampon_case(rose_tampon buf, rose_int idx);
void rose_tampon_pose(rose_tampon buf, rose_int idx, rose_int val);
char *rose_tampon_data(rose_tampon buf);
size_t rose_tampon_size_raw(rose_tampon buf);
static inline void rose_remplir_hasard(rose_tampon buf, rose_tampon alpha) {
    const char *ab = alpha->data();
    const size_t s = alpha->size();
    if (s == 1) {
        buf->assign(buf->size(), ab[0]);
        return;
    }
    if ((s & (s - 1)) == 0 && s <= 256) {
        const size_t m = s - 1;
        unsigned char *p = (unsigned char *)&(*buf)[0];
        const size_t n = buf->size();
        size_t i = 0;
        for (; i + 32 <= n; i += 32) {
            uint64_t r0 = rose_rng_tirer(), r1 = rose_rng_tirer();
            uint64_t r2 = rose_rng_tirer(), r3 = rose_rng_tirer();
            for (int k = 0; k < 8; k++) {
                p[i + k]      = (unsigned char)ab[(r0 >> (8 * k)) & m];
                p[i + 8 + k]  = (unsigned char)ab[(r1 >> (8 * k)) & m];
                p[i + 16 + k] = (unsigned char)ab[(r2 >> (8 * k)) & m];
                p[i + 24 + k] = (unsigned char)ab[(r3 >> (8 * k)) & m];
            }
        }
        for (; i < n; i++) p[i] = (unsigned char)ab[rose_rng_tirer() & m];
    } else if ((s & (s - 1)) == 0) {
        const size_t mask = s - 1;
        const int bits = 64 - __builtin_clzll((unsigned long long)s);
        const size_t per = (size_t)(64 / bits);
        size_t i = 0;
        for (; i + per <= buf->size(); i += per) {
            uint64_t r = rose_rng_tirer();
            for (size_t k = 0; k < per; k++)
                (*buf)[i + k] = ab[(r >> (k * (size_t)bits)) & mask];
        }
        for (; i < buf->size(); i++) (*buf)[i] = ab[rose_rng_tirer() & mask];
    } else {
        for (size_t i = 0; i < buf->size(); i++)
            (*buf)[i] = ab[(size_t)((__uint128_t)rose_rng_tirer() * s >> 64)];
    }
}

// Arguments CLI.
void rose_runtime_set_args(int argc, char **argv);
rose_int rose_nb_arguments();
const char *rose_argument(rose_int i);

// Conversions.

// Temps.
rose_int rose_millisecondes();
rose_ent rose_temps();                     // ns monotones depuis le départ (v3.13)
rose_str rose_duree(const rose_ent &n);    // format adaptatif d'une durée en ns

// Affichage.
void rose_afficher_int(rose_int v);
void rose_afficher_bool(rose_bool v);
void rose_afficher_float(rose_float v);
void rose_afficher_str(const char *s);
void rose_afficher_tampon_longueur(rose_tampon buf);

// Builtins numériques.
rose_int rose_factorielle(rose_int n);
rose_int rose_fibonacci(rose_int n);

// Builtins entiers supplémentaires.
rose_int rose_factorielle(rose_int n);
rose_int rose_fibonacci(rose_int n);
rose_float rose_abs_f(rose_float n);
rose_float rose_min_f(rose_float a, rose_float b);
rose_float rose_max_f(rose_float a, rose_float b);

// Listes (entiers).
typedef std::vector<rose_str> rose_liste;
rose_liste rose_liste_new();
void rose_liste_push(rose_liste &l, const char *v);
void rose_liste_push_gd(rose_liste &l, const rose_gd &v);
void rose_liste_push_q(rose_liste &l, const rose_qd &v);
void rose_liste_push_flt(rose_liste &l, rose_float v);
rose_str rose_liste_concat(rose_liste &l);
rose_int rose_liste_len(rose_liste &l);
rose_str rose_liste_get_str(rose_liste &l, rose_int idx);
rose_str rose_index_str(const rose_str &s, rose_int idx);

// ── Effets et contrôles de fidélité avec l'interprète ─────────────────
rose_int rose_longueur(const rose_str &s);   // caractères UTF-8, pas octets
void rose_quitter(rose_int code);
rose_str rose_lire_texte(const rose_str &chemin);
void rose_ecrire_texte(const rose_str &chemin, const rose_str &texte);
rose_int rose_lire_int();
rose_float rose_lire_flottant();
double rose_flottant_chaine(const std::string &s);   // miroir chaine_vers_flottant
rose_str rose_lire_chaine();
rose_qd rose_lire_rationnel();
rose_int rose_ordre(const rose_str &s);
rose_str rose_sous_chaine(const rose_str &s, rose_int d, rose_int f);
rose_str rose_caractere(rose_int cp);
rose_str rose_texte_q(const rose_qd &q);
// chaine(rationnel) = miroir chaine_de : fraction « a/b » canonique, sans
// plafond (le rendu DECIMAL périodique de ecri est rose_texte_q).
rose_str rose_texte_q_ab(const rose_qd &q);
rose_str rose_texte_flt(rose_float v);
void rose_afficher_liste(const rose_liste &l);

// ══════════════════ M3.4 — interface rose_ent des helpers ══════════════════
void rose_erreur(const std::string &msg);   // « Rose : erreur — … », exit 1

// v3.14 — lecture d'une variable dont la première écriture n'est pas
// garantie à la compilation (affectation seulement dans une branche : le
// dernier écart AOT connu, classe « chemin mort », fermeture de la graine
// générative 200051). Le transpileur suit un drapeau « défini » par
// variable marquée et lit à travers ceci — exactement le message et le
// placement du cœur interprété (v3.13.1). T& (et non const T&) : les sites
// mutateurs (liste.push, table.inserer) partagent le même chemin de garde.
inline void rose_exige(const bool &defini, const char *nom) {
    if (!defini) rose_erreur(std::string("variable '") + nom + "' utilisée sans valeur");
}
template <class T> T& rose_lit(T &v, const bool &defini, const char *nom) {
    rose_exige(defini, nom);
    return v;
}

// Valeurs entières d'utilisateur : rose_ent (machine + promotion GMP).
rose_ent rose_entier(const char *s);
rose_str rose_chaine(const rose_ent &n);
rose_ent rose_pgcd(const rose_ent &a, const rose_ent &b);
rose_ent rose_ppcm(const rose_ent &a, const rose_ent &b);
rose_ent rose_div_florentin(const rose_ent &a, const rose_ent &b);
rose_ent rose_rem_florentin(const rose_ent &a, const rose_ent &b);
rose_ent rose_nb_chiffres(const rose_ent &n);
rose_ent rose_hasard(const rose_ent &n);
rose_ent rose_compter_premiers(const rose_ent &n);
rose_bool rose_est_premier(const rose_ent &n);
rose_ent rose_abs(const rose_ent &n);
rose_ent rose_min(const rose_ent &a, const rose_ent &b);
rose_ent rose_max(const rose_ent &a, const rose_ent &b);
rose_ent rose_entier_q(const rose_qd &q);
rose_ent rose_entier_f(rose_float v);
rose_ent rose_a_entier(const rose_str &s);
rose_ent rose_partie_entiere_f(rose_float v);
rose_ent rose_partie_entiere_q(const rose_qd &q);
void rose_liste_push_int(rose_liste &l, const rose_ent &v);

rose_gd rose_factorielle_gd_impl(const rose_gd &n);
rose_gd rose_fibonacci_gd_impl(const rose_gd &n);
rose_gd rose_puissance_gd_impl(const rose_gd &base, const rose_gd &expo);
// M4.2 : miroir fidele de l'interprete pour pow hors domaine flottant —
// exposant troncque AU PLANCHER par rose_vers_mpz, e<0 → inverse exact,
// 0^negatif et exposant demesure = erreurs runtime aux messages exacts.
rose_qd rose_pow_exact(const rose_qd &base, const rose_gd &expo);
rose_int rose_nb_chiffres_gd_impl(const rose_gd &n);
rose_gd rose_div_florentin_gd_impl(const rose_gd &a, const rose_gd &b);
rose_gd rose_rem_florentin_gd_impl(const rose_gd &a, const rose_gd &b);
rose_gd rose_pgcd_gd_impl(const rose_gd &a, const rose_gd &b);
rose_gd rose_ppcm_gd_impl(const rose_gd &a, const rose_gd &b);
rose_gd rose_abs_gd_impl(const rose_gd &n);
rose_gd rose_min_gd_impl(const rose_gd &a, const rose_gd &b);
rose_gd rose_max_gd_impl(const rose_gd &a, const rose_gd &b);

// Les variantes grands entiers acceptent entiers machine, rose_ent et mpz.
template <class A> inline rose_gd rose_factorielle_gd(const A &n) { return rose_factorielle_gd_impl(rose_vers_mpz(n)); }
template <class A> inline rose_gd rose_fibonacci_gd(const A &n) { return rose_fibonacci_gd_impl(rose_vers_mpz(n)); }
template <class A, class B> inline rose_gd rose_puissance_gd(const A &a, const B &b) { return rose_puissance_gd_impl(rose_vers_mpz(a), rose_vers_mpz(b)); }
template <class A> inline rose_int rose_nb_chiffres_gd(const A &n) { return rose_nb_chiffres_gd_impl(rose_vers_mpz(n)); }
template <class A, class B> inline rose_gd rose_div_florentin_gd(const A &a, const B &b) { return rose_div_florentin_gd_impl(rose_vers_mpz(a), rose_vers_mpz(b)); }
template <class A, class B> inline rose_gd rose_rem_florentin_gd(const A &a, const B &b) { return rose_rem_florentin_gd_impl(rose_vers_mpz(a), rose_vers_mpz(b)); }
template <class A, class B> inline rose_gd rose_pgcd_gd(const A &a, const B &b) { return rose_pgcd_gd_impl(rose_vers_mpz(a), rose_vers_mpz(b)); }
template <class A, class B> inline rose_gd rose_ppcm_gd(const A &a, const B &b) { return rose_ppcm_gd_impl(rose_vers_mpz(a), rose_vers_mpz(b)); }
template <class A> inline rose_gd rose_abs_gd(const A &a) { return rose_abs_gd_impl(rose_vers_mpz(a)); }

/* abs/min/max RATIONNELS — v3.13.1 : le checker laisse passer abs(Q),
   min(Q,Q), min(entier, rationnel)… l'émission doit vivre en mpq, avec
   promotion des entiers (roses génératives 100011/13/14). */
template <class A> inline mpq_class rose_abs_q(const A &a) {
    mpq_class q = rose_q_de(a); return q < 0 ? -q : q;
}
template <class A, class B> inline mpq_class rose_min_q(const A &a, const B &b) {
    mpq_class x = rose_q_de(a), y = rose_q_de(b); return x < y ? x : y;
}
template <class A, class B> inline mpq_class rose_max_q(const A &a, const B &b) {
    mpq_class x = rose_q_de(a), y = rose_q_de(b); return x < y ? y : x;
}

/* div FLORENTIN avec rationnels — v3.13.1 : « entier div rationnel » et
   « rationnel div entier » passent le checker (le quotient planché est un
   entier) ; l'émission doit calculer floor(a/b) en mpq, zéro dénominateur
   rejeté avec le message exact de l'interprète. */
template <class A, class B> inline rose_ent rose_div_florentin_q(const A &a, const B &b) {
    mpq_class y = rose_q_de(b);
    if (mpz_sgn(y.get_num().get_mpz_t()) == 0)
        rose_erreur("division euclidienne par zéro");
    mpq_class q = rose_q_de(a) / y;
    mpz_class f;
    mpz_fdiv_q(f.get_mpz_t(), q.get_num().get_mpz_t(), q.get_den().get_mpz_t());
    return rose_promu(f);
}

/* division FLOTTANTE gardée — v3.13.1 : l'interprète (et les moteurs Rust/Zig)
   rejettent « x / 0.0 » par « division par zéro » ; le '/' brut de l'AOT
   rendait ±inf et poursuivait — divergence silencieuse trouvée par le
   testeur génératif (graine 100036). */
inline rose_float rose_div_f(rose_float a, rose_float b) {
    if (b == 0.0) rose_erreur("division par zéro");
    return a / b;
}
inline rose_float rose_divflo_f(rose_float a, rose_float b) {
    if (b == 0.0) rose_erreur("division euclidienne par zéro");
    return std::floor(a / b);
}
template <class A, class B> inline rose_gd rose_min_gd(const A &a, const B &b) { return rose_min_gd_impl(rose_vers_mpz(a), rose_vers_mpz(b)); }
template <class A, class B> inline rose_gd rose_max_gd(const A &a, const B &b) { return rose_max_gd_impl(rose_vers_mpz(a), rose_vers_mpz(b)); }

// Builtins flottants appelés sur des entiers : conversions explicites exactes.
inline rose_float rose_abs_f(const rose_ent &a) { double x = rose_vers_double(a); return x < 0 ? -x : x; }
inline rose_float rose_min_f(const rose_ent &a, const rose_ent &b) { double x = rose_vers_double(a), y = rose_vers_double(b); return x < y ? x : y; }
inline rose_float rose_min_f(const rose_ent &a, double b) { double x = rose_vers_double(a); return x < b ? x : b; }
inline rose_float rose_min_f(double a, const rose_ent &b) { double y = rose_vers_double(b); return a < y ? a : y; }
inline rose_float rose_max_f(const rose_ent &a, const rose_ent &b) { double x = rose_vers_double(a), y = rose_vers_double(b); return x > y ? x : y; }
inline rose_float rose_max_f(const rose_ent &a, double b) { double x = rose_vers_double(a); return x > b ? x : b; }
inline rose_float rose_max_f(double a, const rose_ent &b) { double y = rose_vers_double(b); return a > y ? a : y; }

// Domaines primitifs (index, tailles, graines) : surcharge rose_ent.
inline rose_tampon rose_tampon_new(const rose_ent &n) {
    if (n.gros() || n.petit() < 0) rose_erreur("tampon : taille impossible");
    return rose_tampon_new((size_t)(long long)n.petit());
}
inline rose_int rose_tampon_case(rose_tampon buf, const rose_ent &idx) {
    if (idx.gros() || idx.petit() < 0 || (size_t)idx.petit() >= buf->size())
        rose_erreur("index hors tampon");
    return rose_tampon_case(buf, (rose_int)idx.petit());
}
inline void rose_tampon_pose(rose_tampon buf, const rose_ent &idx, const rose_ent &val) {
    if (idx.gros() || idx.petit() < 0 || (size_t)idx.petit() >= buf->size())
        rose_erreur("index hors tampon");
    rose_tampon_pose(buf, (rose_int)idx.petit(), (rose_int)val.petit());
}
inline const char *rose_argument(const rose_ent &i) {
    if (i.gros()) return "";
    return rose_argument((rose_int)i.petit());
}
inline void rose_graine(const rose_ent &s) {
    rose_graine((rose_int)(s.gros() ? (int64_t)(unsigned long)mpz_get_ui(s.gd().get_mpz_t()) : s.petit()));
}
inline void rose_afficher_int(const rose_ent &v) {
    if (!v.gros()) { printf("%lld\n", (long long)v.petit()); return; }
    gmp_printf("%Zd\n", v.gd().get_mpz_t());
}
inline rose_str rose_index_str(const rose_str &s, const rose_ent &i) {
    if (i.gros()) return rose_index_str(s, (rose_int)-1);   // hors limites, comme l'interprète
    return rose_index_str(s, (rose_int)i.petit());
}
inline rose_str rose_sous_chaine(const rose_str &s, const rose_ent &d, const rose_ent &f) {
    return rose_sous_chaine(s, (rose_int)(d.gros() ? -1 : d.petit()),
                            (rose_int)(f.gros() ? -1 : f.petit()));
}
inline void rose_liste_push_gd(rose_liste &l, const rose_ent &v) { l.push_back(v.texte()); }
inline rose_ent rose_depuis_double(double v) {   // troncature, comme l'ancien cast (long long)
    if (!(v == v)) return rose_ent(0);
    if (v > -9223372036854777856.0 && v < 9223372036854775808.0) return rose_ent((int64_t)v);
    mpz_class r; mpz_set_d(r.get_mpz_t(), v);
    return rose_promu(r);
}
inline rose_str rose_chaine(const mpz_class &z) { return z.get_str(); }
inline rose_str rose_caractere(const rose_ent &cp) {
    return rose_caractere((rose_int)(cp.gros() ? -1 : cp.petit()));   // hors plage : erreur de l'interprète
}
inline void rose_quitter(const rose_ent &code) {
    rose_quitter((rose_int)(code.gros() ? 0 : code.petit()));
}
inline rose_str rose_liste_get_str(rose_liste &l, const rose_ent &idx) {
    if (idx.gros()) return rose_liste_get_str(l, (rose_int)-1);   // hors limites, comme l'interprète
    return rose_liste_get_str(l, (rose_int)idx.petit());
}
rose_str rose_nombre_pi(rose_int decimales);
inline rose_str rose_nombre_pi(const rose_ent &d) {
    if (d.gros()) rose_erreur("décimales de π hors limites");
    return rose_nombre_pi((rose_int)d.petit());
}

/* ── domaine dynamique des tables (M4.2) ──────────────────────────────
   Le checker type « tout » (T_VALEUR) le résultat de t[cle] et ne
   unifie pas les valeurs des tables : la cellule AOT porte donc son
   propre tag et doit rendre EXACTEMENT comme l'interprete, avec ses
   DEUX facettes documentees :
     * texte()    = miroir chaine_de — rationnel en « a/b », flottant
                    plie en entier sous 1e16 (rendu DANS la table) ;
     * afficher() = miroir ecri — rationnel DECIMAL (longue division),
                    flottant en regle d'affichage (rendu « >> t[k] »).
   Les cles sont textuelles : le transpileur rejette les cles numeriques
   (l'interprete compare par Valeur — « 5 » texte et 5 nombre collident
   en texte ; mieux vaut un rejet poli qu'un silencieux). */
struct rose_dyn {
    int ty = 5;               // miroir T_* : 1 entier, 2 rationnel, 3 flottant, 4 booleen, 5 chaine
    rose_ent e;               // ty 1 (int64 machine avec promotion GMP)
    rose_qd  q;               // ty 2
    rose_float f = 0;         // ty 3
    bool b = false;           // ty 4
    rose_str s;               // ty 5
    std::string texte() const;   // miroir chaine_de
    std::string rendu() const;   // miroir ecri en texte (décimal) — listes/valeurs
    void afficher() const;       // miroir ecri
};
rose_dyn rose_dyn_de(const rose_ent &v);
rose_dyn rose_dyn_de(const rose_gd &v);
rose_dyn rose_dyn_de(const rose_qd &v);
rose_dyn rose_dyn_de(rose_float v);
rose_dyn rose_dyn_de(const rose_str &v);
rose_dyn rose_dyn_de(bool v);
bool rose_dyn_eq(const rose_dyn &a, const rose_dyn &b);        // miroir rose_valeur_egale (branche numerique)
int  rose_dyn_cmp(const rose_dyn &a, const rose_dyn &b);       // miroir cmp_vals

typedef std::vector<std::pair<rose_str, rose_dyn>> rose_table;
rose_table rose_table_new();
void rose_table_inserer(rose_table &t, const rose_str &cle, const rose_dyn &v);
bool rose_table_cle_existe(const rose_table &t, const rose_str &cle);
bool rose_table_supprimer(rose_table &t, const rose_str &cle);
rose_int rose_table_longueur(const rose_table &t);
rose_dyn rose_table_get(const rose_table &t, const rose_str &cle);   // « clé absente de la table »
rose_liste rose_table_cles(const rose_table &t);                     // ordre d'insertion
rose_liste rose_table_valeurs(const rose_table &t);                   // rendues par afficher (miroir ecri)
void rose_afficher_table(const rose_table &t);                        // "[cle => val, ...]"
bool rose_table_eq(const rose_table &a, const rose_table &b);         // ordre independant (miroir)

#endif  /* ROSE_RUNTIME_H */
