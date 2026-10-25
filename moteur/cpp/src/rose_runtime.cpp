#include "rose_runtime.h"
#include <cstdint>

uint64_t g_rng_etat = (uint64_t)getpid() * 0x9E3779B97F4A7C15ULL
                     ^ (uint64_t)(uintptr_t)&g_rng_etat;
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <string>
#include <memory>
#include <vector>
#include <unistd.h>
#include <cmath>
#include <fstream>
#include <sys/random.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>

#include <sstream>
#include <cstring>

std::string rose_ent::texte() const {
    if (!g_) return std::to_string(m_);
    return g_->get_str();
}

// ── Erreurs : format et comportement de l'interprète (erreur_runtime) ──
void rose_erreur(const std::string &msg) {
    std::fflush(stdout);
    std::fprintf(stderr, "Rose : erreur — %s\n", msg.c_str());
    std::exit(1);
}

// utf8 : mêmes helpers que l'interprète (comptage par octet de tête)
static size_t rose_utf8_seq(unsigned char c) {
    return (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 :
           (c & 0xE0) == 0xC0 ? 2 : 1;
}
static size_t rose_utf8_len(const std::string &s) {
    size_t k = 0;
    for (size_t i = 0; i < s.size(); i++) if ((s[i] & 0xC0) != 0x80) k++;
    return k;
}
static size_t rose_utf8_off(const std::string &s, size_t k) {
    size_t vu = 0;
    for (size_t i = 0; i < s.size(); i++)
        if ((s[i] & 0xC0) != 0x80) { if (vu == k) return i; vu++; }
    return s.size();
}
static unsigned long rose_utf8_char_at(const std::string &s, size_t k) {
    size_t o = rose_utf8_off(s, k);
    unsigned char c = (unsigned char)s[o];
    size_t q = rose_utf8_seq(c);
    unsigned long v = (q == 1) ? c : (c & (0xFFu >> (q + 1)));
    for (size_t i = 1; i < q && o + i < s.size(); i++)
        v = (v << 6) | ((unsigned char)s[o + i] & 0x3F);
    return v;
}
static std::string rose_utf8_char_str(const std::string &s, size_t k) {
    size_t o = rose_utf8_off(s, k);
    return s.substr(o, rose_utf8_seq((unsigned char)s[o]));
}
static std::string rose_utf8_encode(unsigned long cp) {
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
// ── Tampons ─────────────────────────────────────────────────────────────
rose_tampon rose_tampon_new(size_t n) {
    return std::make_shared<std::string>(n, '\0');
}

rose_tampon rose_octets(const char *s) {
    return std::make_shared<std::string>(s);
}

rose_int rose_tampon_longueur(rose_tampon buf) {
    return (rose_int)buf->size();
}

rose_int rose_tampon_case(rose_tampon buf, rose_int idx) {
    return (unsigned char)(*buf)[(size_t)idx];
}

char *rose_tampon_data(rose_tampon buf) { return buf->data(); }
size_t rose_tampon_size_raw(rose_tampon buf) { return buf->size(); }

void rose_tampon_pose(rose_tampon buf, rose_int idx, rose_int val) {
    (*buf)[(size_t)idx] = (char)val;
}
// ── Arguments CLI ────────────────────────────────────────────────────────
static std::vector<std::string> g_args;

void rose_runtime_set_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) g_args.push_back(argv[i]);
}

rose_int rose_nb_arguments() {
    return (rose_int)g_args.size();
}

const char *rose_argument(rose_int i) {
    if (i < 0 || i >= (rose_int)g_args.size()) return "";
    return g_args[i].c_str();
}

// ── Conversions ─────────────────────────────────────────────────────────
rose_ent rose_entier(const char *s) {
    // miroir de chaine_vers_entier de l'interprète : entier strict, fraction
    // (plancher), ou décimale tronquée à la partie avant le point.
    std::string ch(s);
    mpz_class r;
    if (mpz_set_str(r.get_mpz_t(), ch.c_str(), 10) == 0) return rose_promu(r);
    size_t p = ch.find('/');
    if (p != std::string::npos) {
        mpz_class n, d;
        if (mpz_set_str(n.get_mpz_t(), ch.substr(0, p).c_str(), 10)
                || mpz_set_str(d.get_mpz_t(), ch.substr(p + 1).c_str(), 10)
                || mpz_sgn(d.get_mpz_t()) == 0)
            rose_erreur("conversion chaine → entier impossible");
        mpz_fdiv_q(r.get_mpz_t(), n.get_mpz_t(), d.get_mpz_t());
        return rose_promu(r);
    }
    size_t pt = ch.find('.');
    if (pt != std::string::npos)
        if (mpz_set_str(r.get_mpz_t(), ch.substr(0, pt).c_str(), 10) == 0)
            return rose_promu(r);
    rose_erreur("conversion chaine → entier impossible");
    return rose_ent(0);
}

rose_str rose_chaine(const rose_ent &n) {
    return n.texte();
}

// ── Temps ───────────────────────────────────────────────────────────────
rose_int rose_millisecondes() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (rose_int)(t.tv_sec * 1000LL + t.tv_nsec / 1000000);
}

// ── Chronomètre v3.13 — miroir exact des temps()/duree() de l'interprète ──
static std::pair<std::string, uint64_t> div_decimale_u64(const std::string &n_str, uint64_t div) {
    std::string q;
    uint64_t r = 0;
    for (char c : n_str) {
        uint64_t cour = r * 10 + (uint64_t)(c - '0');
        q.push_back((char)('0' + cour / div));
        r = cour % div;
    }
    size_t z = q.find_first_not_of('0');
    return {z == std::string::npos ? std::string("0") : q.substr(z), r};
}

rose_ent rose_temps() {
    static const std::chrono::steady_clock::time_point depart =
            std::chrono::steady_clock::now();
    long long ns = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - depart).count();
    return rose_ent(ns);
}

rose_str rose_duree(const rose_ent &n) {
    if (n.gros() ? mpz_sgn(n.gd().get_mpz_t()) < 0 : n.petit() < 0)
        rose_erreur("duree : la durée doit être positive ou nulle");
    std::string n_str = n.gros() ? n.gd().get_str() : std::to_string((long long)n.petit());
    static const char *unit[] = {" ns", " µs", " ms", " s", " min", " h"};
    static const uint64_t scale[] = {1ULL, 1000ULL, 1000000ULL, 1000000000ULL,
                                     60000000000ULL, 3600000000000ULL};
    size_t z = n_str.find_first_not_of('0');
    n_str = z == std::string::npos ? "0" : n_str.substr(z);
    auto au_moins = [&](uint64_t s) {
        std::string t = std::to_string(s);
        return n_str.size() != t.size() ? n_str.size() > t.size() : n_str >= t;
    };
    int u = 0;
    for (int i = 1; i <= 5; i++) if (au_moins(scale[i])) u = i;
    auto [part, reste] = div_decimale_u64(n_str, scale[u]);
    if (u == 0 || reste == 0) return part + unit[u];
    uint64_t frac3 = (uint64_t)((reste * 1000ULL) / scale[u]);
    std::string fs = std::to_string(frac3);
    while (fs.size() < 3) fs = "0" + fs;
    while (!fs.empty() && fs.back() == '0') fs.pop_back();
    return part + "." + fs + unit[u];
}

// ── Affichage ───────────────────────────────────────────────────────────
void rose_afficher_int(rose_int v) {
    printf("%ld\n", (long)v);
}

void rose_afficher_bool(rose_bool v) {
    printf("%s\n", v ? "vrai" : "faux");
}

// Représentation la plus courte qui relit exactement (miroir de
// texte_flottant de l'interprète), puis rendu comme ecri().
static std::string rose_texte_flottant(double d) {
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

void rose_afficher_float(rose_float v) {
    double d = v;
    if (d != d) { printf("non_nombre\n"); return; }
    if (d == 1.0 / 0.0) { printf("infini\n"); return; }
    if (d == -1.0 / 0.0) { printf("-infini\n"); return; }
    if (d == std::floor(d) && std::fabs(d) < 1e16) { printf("%lld\n", (long long)d); return; }
    printf("%s\n", rose_texte_flottant(d).c_str());
}

void rose_afficher_str(const char *s) {
    printf("%s\n", s);
}

void rose_afficher_tampon_longueur(rose_tampon buf) {
    printf("%zu\n", buf->size());
}

// ── Builtins numériques ────────────────────────────────────────────────
#include <gmp.h>
#include <gmpxx.h>

rose_ent rose_pgcd(const rose_ent &a, const rose_ent &b) {
    if (!a.gros() && !b.gros() && a.petit() != INT64_MIN && b.petit() != INT64_MIN) {
        uint64_t ua = (uint64_t)(a.petit() < 0 ? -a.petit() : a.petit());
        uint64_t ub = (uint64_t)(b.petit() < 0 ? -b.petit() : b.petit());
        while (ub) { uint64_t t = ua % ub; ua = ub; ub = t; }
        return rose_ent((int64_t)ua);
    }
    mpz_class r, za = rose_vers_mpz(a), zb = rose_vers_mpz(b);
    mpz_gcd(r.get_mpz_t(), za.get_mpz_t(), zb.get_mpz_t());
    return rose_promu(r);
}

rose_ent rose_ppcm(const rose_ent &a, const rose_ent &b) {
    mpz_class r, za = rose_vers_mpz(a), zb = rose_vers_mpz(b);
    mpz_lcm(r.get_mpz_t(), za.get_mpz_t(), zb.get_mpz_t());
    return rose_promu(r);   // le ppcm peut dépasser int64 : promotion fidèle
}

// factorielle : arbre de produits (miroir de l'interprète rose.cpp).
static void rose_factorielle_arbre(mpz_class &dest, unsigned long lo, unsigned long hi) {
    if (lo > hi) { dest = 1; return; }
    if (hi - lo < 16) {
        dest = lo;
        for (unsigned long x = lo + 1; x <= hi; x++) mpz_mul_ui(dest.get_mpz_t(), dest.get_mpz_t(), x);
        return;
    }
    unsigned long mid = lo + (hi - lo) / 2;
    mpz_class g, d;
    rose_factorielle_arbre(g, lo, mid);
    rose_factorielle_arbre(d, mid + 1, hi);
    dest = g * d;
}
rose_gd rose_factorielle_gd_impl(const rose_gd &zn) {
    if (mpz_sgn(zn.get_mpz_t()) < 0) rose_erreur("factorielle d'un négatif : impossible");
    if (!mpz_fits_ulong_p(zn.get_mpz_t()) || mpz_get_ui(zn.get_mpz_t()) > 0xFFFFFFFFUL) return 0;
    mpz_class r;
    rose_factorielle_arbre(r, 1, mpz_get_ui(zn.get_mpz_t()));
    return r;
}

// fibonacci : doublement rapide (miroir de l'interprète rose.cpp).
rose_gd rose_fibonacci_gd_impl(const rose_gd &zn) {
    if (mpz_sgn(zn.get_mpz_t()) < 0) return 0;
    mpz_class a = 0, b = 1, t1, t2, t3;
    mpz_class nn = zn;
    mp_bitcnt_t bits = mpz_sizeinbase(nn.get_mpz_t(), 2);
    for (mp_bitcnt_t i = bits; i-- > 0;) {
        mpz_mul_ui(t1.get_mpz_t(), b.get_mpz_t(), 2);
        mpz_sub(t1.get_mpz_t(), t1.get_mpz_t(), a.get_mpz_t());
        mpz_mul(t1.get_mpz_t(), t1.get_mpz_t(), a.get_mpz_t());
        mpz_mul(t2.get_mpz_t(), a.get_mpz_t(), a.get_mpz_t());
        mpz_mul(t3.get_mpz_t(), b.get_mpz_t(), b.get_mpz_t());
        mpz_add(t2.get_mpz_t(), t2.get_mpz_t(), t3.get_mpz_t());
        if (mpz_tstbit(nn.get_mpz_t(), i)) {
            a = t2;
            mpz_add(b.get_mpz_t(), t1.get_mpz_t(), t2.get_mpz_t());
        } else { a = t1; b = t2; }
    }
    return a;
}

// puissance entière : base^expo, expo >= 0.
rose_gd rose_puissance_gd_impl(const rose_gd &base, const rose_gd &expo) {
    if (mpz_sgn(expo.get_mpz_t()) < 0) return 0;
    if (!mpz_fits_ulong_p(expo.get_mpz_t())) return 0;   // exposant astronomique
    mpz_class r;
    mpz_pow_ui(r.get_mpz_t(), base.get_mpz_t(), mpz_get_ui(expo.get_mpz_t()));
    return r;
}

// M4.2 — miroir exact de la branche non-flottante du pow interprete (rose.cpp) :
// q^e et z^e exacts, e negatif par inversion, messages d'erreur identiques.
rose_qd rose_pow_exact(const rose_qd &base, const rose_gd &expo) {
    if (!mpz_fits_slong_p(expo.get_mpz_t()))
        rose_erreur("exposant démesuré (la puissance exploserait au-delà de tout calcul)");
    long e = mpz_get_si(expo.get_mpz_t());
    mpz_class num = base.get_num(), den = base.get_den();
    if (e < 0) {
        if (mpz_sgn(num.get_mpz_t()) == 0) rose_erreur("0^exposant négatif");
        std::swap(num, den);
        e = -e;
    }
    mpz_pow_ui(num.get_mpz_t(), num.get_mpz_t(), (unsigned long)e);
    mpz_pow_ui(den.get_mpz_t(), den.get_mpz_t(), (unsigned long)e);
    rose_qd r(num, den);
    r.canonicalize();
    return r;
}

rose_int rose_nb_chiffres_gd_impl(const rose_gd &n) {
    if (mpz_sgn(n.get_mpz_t()) == 0) return 1;
    rose_gd t; mpz_abs(t.get_mpz_t(), n.get_mpz_t());
    return (rose_int)mpz_sizeinbase(t.get_mpz_t(), 10);   // quirks GMP = sémantique Rose
}

void rose_afficher_gd(const rose_gd &n) {
    gmp_printf("%Zd\n", n.get_mpz_t());
}

rose_bool rose_est_premier(const rose_ent &n) {
    if (!n.gros()) {
        int64_t v = n.petit();
        if (v <= 1) return false;
        for (int64_t i = 2; i <= v / i; i++) if (v % i == 0) return false;
        return true;
    }
    return mpz_probab_prime_p(rose_vers_mpz(n).get_mpz_t(), 25) != 0;
}

rose_ent rose_compter_premiers(const rose_ent &n) {
    if (n.gros()) rose_erreur("compter_premiers : bornage hors machine");
    int64_t v = n.petit();
    if (v < 2) return rose_ent(0);
    std::vector<unsigned char> crible((size_t)v + 1, 0);
    int64_t compte = 0;
    for (int64_t i = 2; i <= v; i++) {
        if (!crible[(size_t)i]) {
            compte++;
            if (i <= v / i)
                for (int64_t j = i * i; j <= v; j += i) crible[(size_t)j] = 1;
        }
    }
    return rose_ent(compte);
}

rose_ent rose_nb_chiffres(const rose_ent &n) {
    mpz_class z = rose_vers_mpz(n);
    if (mpz_sgn(z.get_mpz_t()) == 0) return rose_ent(1);
    mpz_abs(z.get_mpz_t(), z.get_mpz_t());
    return rose_ent((int64_t)mpz_sizeinbase(z.get_mpz_t(), 10));   // quirks GMP = sémantique Rose
}

rose_ent rose_hasard(const rose_ent &n) {
    if (!n.gros()) {
        int64_t nn = n.petit();
        if (nn <= 0) rose_erreur("hasard : bornage positif attendu");
        if ((nn & (nn - 1)) == 0) return rose_ent((int64_t)(rose_rng_tirer() & (uint64_t)(nn - 1)));
        return rose_ent((int64_t)((__uint128_t)rose_rng_tirer() * (uint64_t)nn >> 64));
    }
    if (mpz_sgn(n.gd().get_mpz_t()) <= 0) rose_erreur("hasard : bornage positif attendu");
    // miroir de l'interprète : un seul tirage 64 bits réduit modulo n
    uint64_t r64 = rose_rng_tirer();
    mpz_class rr;
    mpz_set_ui(rr.get_mpz_t(), (unsigned long)(r64 >> 32));
    mpz_mul_2exp(rr.get_mpz_t(), rr.get_mpz_t(), 32);
    mpz_add_ui(rr.get_mpz_t(), rr.get_mpz_t(), (unsigned long)(r64 & 0xFFFFFFFFu));
    mpz_mod(rr.get_mpz_t(), rr.get_mpz_t(), n.gd().get_mpz_t());
    return rose_ent(rr);
}

rose_ent rose_abs(const rose_ent &n) {
    if (!n.gros() && n.petit() != INT64_MIN)
        return rose_ent(n.petit() < 0 ? -n.petit() : n.petit());
    mpz_class r;
    mpz_abs(r.get_mpz_t(), rose_vers_mpz(n).get_mpz_t());
    return rose_promu(r);
}
rose_ent rose_min(const rose_ent &a, const rose_ent &b) { return b < a ? b : a; }
rose_ent rose_max(const rose_ent &a, const rose_ent &b) { return a < b ? b : a; }
rose_float rose_abs_f(rose_float n) { return n < 0 ? -n : n; }
rose_float rose_min_f(rose_float a, rose_float b) { return a < b ? a : b; }
rose_float rose_max_f(rose_float a, rose_float b) { return a > b ? a : b; }

// ── Listes ──────────────────────────────────────────────────────────────
rose_liste rose_liste_new() { return rose_liste(); }
rose_str rose_liste_get_str(rose_liste &l, rose_int idx) { if (idx < 0 || idx >= (rose_int)l.size()) rose_erreur("index hors des limites de la liste"); return l[(size_t)idx]; }
rose_int rose_liste_len(rose_liste &l) { return (rose_int)l.size(); }

// ── Listes et chaînes ───────────────────────────────────────────────────
void rose_liste_push(rose_liste &l, const char *v) { l.push_back(v); }
rose_str rose_liste_concat(rose_liste &l) {
    rose_str r;
    for (auto &s : l) r += s;
    return r;
}
rose_str rose_index_str(const rose_str &s, rose_int idx) {
    if (idx < 0 || (size_t)idx >= rose_utf8_len(s))
        rose_erreur("index hors des limites de la chaîne");
    return rose_utf8_char_str(s, (size_t)idx);
}

// ── Rationnels exacts et divisions euclidiennes ──────────────────────
rose_qd rose_q_div(const rose_qd &a, const rose_qd &b) {
    if (mpq_sgn(b.get_mpq_t()) == 0) { rose_erreur("division par zéro"); }
    return a / b;
}

int rose_q_cmp(const rose_qd &a, const rose_qd &b) {
    return mpq_cmp(a.get_mpq_t(), b.get_mpq_t());
}

bool rose_approx_q(const rose_qd &a, const rose_qd &b, bool au_moins_un_flottant) {
    if (!au_moins_un_flottant) return mpq_cmp(a.get_mpq_t(), b.get_mpq_t()) == 0;
    double x = mpq_get_d(a.get_mpq_t()), y = mpq_get_d(b.get_mpq_t());
    double ecart = x > y ? x - y : y - x;
    double echelle = (x > 0 ? x : -x) > (y > 0 ? y : -y) ? (x > 0 ? x : -x) : (y > 0 ? y : -y);
    if (echelle < 1.0) echelle = 1.0;
    return ecart <= 1e-9 * echelle;
}

bool rose_approx_f(rose_float a, rose_float b) {
    double ecart = a > b ? a - b : b - a;
    double echelle = (a > 0 ? a : -a) > (b > 0 ? b : -b) ? (a > 0 ? a : -a) : (b > 0 ? b : -b);
    if (echelle < 1.0) echelle = 1.0;
    return ecart <= 1e-9 * echelle;
}

rose_ent rose_entier_q(const rose_qd &q) {
    mpz_class r;
    mpz_fdiv_q(r.get_mpz_t(), q.get_num().get_mpz_t(), q.get_den().get_mpz_t());
    return rose_promu(r);
}

// Écriture exacte avec périodes entre crochets (miroir de ecri de l'interprète).
std::string rose_texte_q(const rose_qd &q_in) {
    std::string out_str;
    {
    mpq_class q = q_in;
    q.canonicalize();
    if (q.get_den() == 1) { return q.get_num().get_str().c_str(); }
    if (mpz_sizeinbase(q.get_num().get_mpz_t(), 10)
            + mpz_sizeinbase(q.get_den().get_mpz_t(), 10) > 500) {
        return std::string(q.get_num().get_str().c_str()) + "/" + q.get_den().get_str().c_str();
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
    out_str = entier.get_str().c_str();
    if (ndig) {
        out_str += ".";
        out_str.append(digits, (size_t)(peri >= 0 ? peri : ndig));
        if (peri >= 0) {
            out_str += "[";
            out_str.append(digits + peri, (size_t)(ndig - peri));
            out_str += "]";
        } else if (ndig >= 200) {
            out_str += "…";
        }
    }
    }
    return out_str;
}
void rose_afficher_q(const rose_qd &q) {
    printf("%s\n", rose_texte_q(q).c_str());
}

rose_ent rose_div_florentin(const rose_ent &a, const rose_ent &b) {
    if (!b.gros() && b.petit() == 0) rose_erreur("division euclidienne par zéro");
    if (!a.gros() && !b.gros() && !(a.petit() == INT64_MIN && b.petit() == -1)) {
        int64_t q = a.petit() / b.petit(), r = a.petit() % b.petit();
        if (r != 0 && ((r < 0) != (b.petit() < 0))) q--;   // vers le plancher
        return rose_ent(q);
    }
    mpz_class x = rose_vers_mpz(a), y = rose_vers_mpz(b), r;
    mpz_fdiv_q(r.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
    return rose_promu(r);
}

rose_ent rose_rem_florentin(const rose_ent &a, const rose_ent &b) {
    if (!b.gros() && b.petit() == 0) rose_erreur("modulo par zéro");
    if (!a.gros() && !b.gros() && !(a.petit() == INT64_MIN && b.petit() == -1)) {
        int64_t r = a.petit() % b.petit();
        if (r != 0 && ((r < 0) != (b.petit() < 0))) r += b.petit();   // reste du plancher
        return rose_ent(r);
    }
    mpz_class x = rose_vers_mpz(a), y = rose_vers_mpz(b), r;
    mpz_fdiv_r(r.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
    return rose_promu(r);
}

// Rendu texte d'un flottant (miroir de ecri de l'interprète).
std::string rose_texte_flt(rose_float v) {
    double d = v;
    if (d != d) return "non_nombre";
    if (d == 1.0 / 0.0) return "infini";
    if (d == -1.0 / 0.0) return "-infini";
    if (d == std::floor(d) && std::fabs(d) < 1e16) {
        char buf[32]; snprintf(buf, sizeof buf, "%lld", (long long)d);
        return std::string(buf);
    }
    return rose_texte_flottant(d);
}

void rose_liste_push_int(rose_liste &l, const rose_ent &v) { l.push_back(v.texte()); }
void rose_liste_push_gd(rose_liste &l, const rose_gd &v) { l.push_back(v.get_str().c_str()); }
void rose_liste_push_q(rose_liste &l, const rose_qd &v) { l.push_back(rose_texte_q(v)); }
void rose_liste_push_flt(rose_liste &l, rose_float v) { l.push_back(rose_texte_flt(v)); }

// Chaîne → entier (lecture des éléments d'une liste d'entiers).
rose_ent rose_a_entier(const rose_str &s) {
    mpz_class r;
    if (mpz_set_str(r.get_mpz_t(), s.c_str(), 10)) return rose_ent(0);
    return rose_promu(r);
}

rose_ent rose_entier_f(rose_float v) {   // troncature (mpz_set_d)
    if (!(v == v)) return rose_ent(0);
    if (v > -9223372036854777856.0 && v < 9223372036854775808.0) return rose_ent((int64_t)v);
    mpz_class r; mpz_set_d(r.get_mpz_t(), v);   // mpz_set_d tronque déjà
    return rose_promu(r);
}

void rose_afficher_liste(const rose_liste &l) {
    putchar('[');
    for (size_t i = 0; i < l.size(); i++) {
        if (i) fputs(", ", stdout);
        fputs(l[i].c_str(), stdout);
    }
    fputs("]\n", stdout);
}

// ── Builtin nombre_pi : π avec N décimales (formule de Machin, miroir) ──
rose_str rose_nombre_pi(rose_int decimales) {
    unsigned long chiffres = (unsigned long)decimales;
    if (chiffres == 0 || chiffres > 2000000) { rose_erreur("décimales de π hors limites (1..2000000)"); }
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

// partie_entiere : plancher (miroir de l'interprète).
rose_ent rose_partie_entiere_f(rose_float v) {
    if (!(v == v)) return rose_ent(0);
    double f = std::floor(v);
    if (f > -9223372036854777856.0 && f < 9223372036854775808.0) return rose_ent((int64_t)f);
    mpz_class r; mpz_set_d(r.get_mpz_t(), f);   // f déjà entière : conversion exacte
    return rose_promu(r);
}
rose_ent rose_partie_entiere_q(const rose_qd &q) { return rose_entier_q(q); }

// Division euclidienne (plancher) sur grands entiers.
rose_gd rose_div_florentin_gd_impl(const rose_gd &a, const rose_gd &b) {
    if (mpz_sgn(b.get_mpz_t()) == 0) { rose_erreur("division euclidienne par zéro"); }
    mpz_class r;
    mpz_fdiv_q(r.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
    return r;
}

// Reste euclidien (plancher) sur grands entiers.
rose_gd rose_rem_florentin_gd_impl(const rose_gd &a, const rose_gd &b) {
    if (mpz_sgn(b.get_mpz_t()) == 0) { rose_erreur("modulo par zéro"); }
    mpz_class r;
    mpz_fdiv_r(r.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
    return r;
}

// Variantes grands entiers exactes (GMP) — les arguments int64 se convertissent.
rose_gd rose_pgcd_gd_impl(const rose_gd &a, const rose_gd &b) {
    rose_gd r; mpz_gcd(r.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t()); return r;
}
rose_gd rose_ppcm_gd_impl(const rose_gd &a, const rose_gd &b) {
    rose_gd r; mpz_lcm(r.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t()); return r;
}
rose_gd rose_abs_gd_impl(const rose_gd &n) {
    rose_gd r; mpz_abs(r.get_mpz_t(), n.get_mpz_t()); return r;
}
rose_gd rose_min_gd_impl(const rose_gd &a, const rose_gd &b) { return a < b ? a : b; }
rose_gd rose_max_gd_impl(const rose_gd &a, const rose_gd &b) { return a > b ? a : b; }

rose_int rose_longueur(const rose_str &s) {
    return (rose_int)rose_utf8_len(s);
}

// ── Effets et contrôles de fidélité (M3.3bis) ──────────────────────────
void rose_quitter(rose_int code) {
    std::fflush(stdout);
    std::exit((int)code);
}
rose_str rose_lire_texte(const rose_str &chemin) {
    std::ifstream in(chemin, std::ios::binary);
    if (!in) rose_erreur("lecture du fichier impossible (" + chemin + ")");
    std::stringstream ss; ss << in.rdbuf();
    if (ss.str().size() > ((size_t)1 << 28))
        rose_erreur("fichier trop volumineux (" + chemin + ")");
    return ss.str();
}
void rose_ecrire_texte(const rose_str &chemin, const rose_str &texte) {
    std::ofstream out(chemin, std::ios::binary | std::ios::trunc);
    if (!out) rose_erreur("écriture du fichier impossible (" + chemin + ")");
    out << texte;
    out.flush();
    if (!out) rose_erreur("écriture du fichier impossible (" + chemin + ")");
}
static std::string rose_lire_ligne() {
    char buf[4096];
    if (!std::fgets(buf, sizeof buf, stdin)) rose_erreur("entrée attendue, fichier fermé");
    size_t l = std::strlen(buf);
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r' || buf[l - 1] == ' ')) buf[--l] = 0;
    return std::string(buf, l);
}
rose_int rose_lire_int() {   // miroir de lire_selon (entier) de l'interprète
    std::string t = rose_lire_ligne();
    mpz_class r;
    if (t.find('/') != std::string::npos) {
        mpq_class q;
        if (mpq_set_str(q.get_mpq_t(), t.c_str(), 10) == 0) {
            q.canonicalize();
            mpz_tdiv_q(r.get_mpz_t(), q.get_num().get_mpz_t(), q.get_den().get_mpz_t());
            return (rose_int)mpz_get_si(r.get_mpz_t());
        }
    }
    if (t.find('.') != std::string::npos) {
        mpz_set_d(r.get_mpz_t(), std::strtod(t.c_str(), nullptr));
        return (rose_int)mpz_get_si(r.get_mpz_t());
    }
    if (mpz_set_str(r.get_mpz_t(), t.c_str(), 10)) rose_erreur("nombre entier invalide");
    return (rose_int)mpz_get_si(r.get_mpz_t());
}
rose_float rose_lire_flottant() {
    std::string t = rose_lire_ligne();
    char *fin = nullptr;
    double d = std::strtod(t.c_str(), &fin);
    if (fin == t.c_str()) rose_erreur("flottant invalide");
    return d;
}
double rose_flottant_chaine(const std::string &s) {
    // miroir exact de chaine_vers_flottant (rose.cpp) : strtod, « infini »
    // explicite, erreur sinon.
    char *fin = nullptr;
    double d = std::strtod(s.c_str(), &fin);
    if (fin == s.c_str()) {
        if (s == "infini") return 1.0 / 0.0;
        rose_erreur("conversion chaine → flottant impossible");
    }
    return d;
}
rose_str rose_lire_chaine() { return rose_lire_ligne(); }
rose_qd rose_lire_rationnel() {
    std::string t = rose_lire_ligne();
    rose_qd q;
    if (mpq_set_str(q.get_mpq_t(), t.c_str(), 10)) rose_erreur("rationnel invalide");
    q.canonicalize();
    return q;
}
rose_int rose_ordre(const rose_str &s) {
    if (rose_utf8_len(s) != 1)
        rose_erreur("ordre attend une chaîne d'un seul caractère");
    return (rose_int)rose_utf8_char_at(s, 0);
}
rose_str rose_sous_chaine(const rose_str &s, rose_int d, rose_int f) {
    if (d < 0 || f < 0) rose_erreur("sous_chaine : les bornes doivent être des entiers positifs");
    size_t taille = rose_utf8_len(s);
    if ((size_t)d > taille || (size_t)f < (size_t)d) rose_erreur("sous_chaine : bornes invalides");
    size_t ff = ((size_t)f > taille) ? taille : (size_t)f;
    size_t o1 = rose_utf8_off(s, (size_t)d);
    size_t o2 = (ff == taille) ? s.size() : rose_utf8_off(s, ff);
    return s.substr(o1, o2 - o1);
}
rose_str rose_caractere(rose_int cp) {
    if (cp < 0 || cp > 0x10FFFF) rose_erreur("caractere : code hors plage (0 à 1114111)");
    if (cp >= 0xD800 && cp <= 0xDFFF) rose_erreur("caractere : code réservé (55296 à 57343)");
    return rose_utf8_encode((unsigned long)cp);
}

/* ════════════════════════════════════════════════════════════════════
   Domaine dynamique des tables — M4.2. Chaque rendu est un miroir :
   texte() reproduit chaine_de (rose.cpp), afficher() reproduit ecri,
   eq/cmp reproduisent rose_valeur_egale/cmp_vals (croisades numeriques
   exactes : F par vers_d, Q par mpq, sinon mpz).
   ════════════════════════════════════════════════════════════════════ */
std::string rose_dyn::texte() const {          // miroir chaine_de
    switch (ty) {
        case 1: return e.texte();
        case 2: { rose_qd c = q; c.canonicalize();
                  if (mpz_get_si(c.get_den().get_mpz_t()) == 1 && mpz_fits_slong_p(c.get_den().get_mpz_t()))
                      return c.get_num().get_str();
                  return c.get_num().get_str() + "/" + c.get_den().get_str(); }
        case 3: return rose_texte_flt(f);       // ecri==chaine_de pour les flottants
        case 4: return b ? "vrai" : "faux";
        default: return s;
    }
}
std::string rose_dyn::rendu() const {          // miroir ecri en texte — rendu des listes/valeurs
    switch (ty) {
        case 1: return e.texte();
        case 2: return rose_texte_q(q);        // DECIMAL (les listes rendent par ecri)
        case 3: return rose_texte_flt(f);
        case 4: return b ? "vrai" : "faux";
        default: return s;
    }
}
void rose_dyn::afficher() const {              // miroir ecri (l'op afficher ajoute la ligne)
    switch (ty) {
        case 1: rose_afficher_int(e); return;
        case 2: rose_afficher_q(q); return;    // DECIMAL, caps 200/500 inclus
        case 3: { std::string r = rose_texte_flt(f); std::fputs(r.c_str(), stdout); break; }
        case 4: std::fputs(b ? "vrai" : "faux", stdout); break;
        default: std::fputs(s.c_str(), stdout); break;
    }
    std::fputs("\n", stdout);
}

rose_dyn rose_dyn_de(const rose_ent &v)  { rose_dyn d; d.ty = 1; d.e = v; return d; }rose_dyn rose_dyn_de(const rose_gd &v)   { rose_dyn d; d.ty = 1; d.e = rose_ent(v); return d; }
rose_dyn rose_dyn_de(const rose_qd &v)   { rose_dyn d; d.ty = 2; d.q = v; return d; }
rose_dyn rose_dyn_de(rose_float v)       { rose_dyn d; d.ty = 3; d.f = v; return d; }
rose_dyn rose_dyn_de(const rose_str &v)  { rose_dyn d; d.ty = 5; d.s = v; return d; }
rose_dyn rose_dyn_de(bool v)             { rose_dyn d; d.ty = 4; d.b = v; return d; }

static double dyn_vers_d(const rose_dyn &v) {          // miroir vers_d
    switch (v.ty) {
        case 1: return rose_vers_double(v.e);
        case 2: return mpq_get_d(v.q.get_mpq_t());
        case 3: return v.f;
        case 4: return v.b ? 1.0 : 0.0;
        default: rose_erreur("conversion vers flottant impossible"); return 0;
    }
}
static rose_qd dyn_vers_q(const rose_dyn &v) {         // miroir vers_q
    switch (v.ty) {
        case 1: return rose_q_de(v.e);
        case 2: return v.q;
        case 3: { rose_qd r; mpq_set_d(r.get_mpq_t(), v.f); r.canonicalize(); return r; }
        case 4: return rose_qd(v.b ? 1 : 0);
        default: rose_erreur("conversion vers rationnel impossible"); return rose_qd();
    }
}
static rose_gd dyn_vers_z(const rose_dyn &v) {         // miroir vers_z (plancheur sur Q)
    switch (v.ty) {
        case 1: return rose_vers_mpz(v.e);
        case 2: return rose_vers_mpz(v.q);
        case 3: { rose_gd r; mpz_set_d(r.get_mpz_t(), v.f); return r; }
        case 4: return rose_gd(v.b ? 1 : 0);
        default: rose_erreur("conversion vers entier impossible"); return rose_gd();
    }
}

int rose_dyn_cmp(const rose_dyn &a, const rose_dyn &b) {   // miroir cmp_vals
    if (a.ty == 5 && b.ty == 5) return a.s == b.s ? 0 : (a.s < b.s ? -1 : 1);
    if (a.ty == 3 || b.ty == 3) { double A = dyn_vers_d(a), B = dyn_vers_d(b);
                                  return A < B ? -1 : (A > B ? 1 : 0); }
    if (a.ty == 2 || b.ty == 2) { rose_qd A = dyn_vers_q(a), B = dyn_vers_q(b);
                                  return mpq_cmp(A.get_mpq_t(), B.get_mpq_t()); }
    rose_gd A = dyn_vers_z(a), B = dyn_vers_z(b);
    return mpz_cmp(A.get_mpz_t(), B.get_mpz_t());
}
bool rose_dyn_eq(const rose_dyn &a, const rose_dyn &b) {   // miroir rose_valeur_egale
    if (a.ty == 5 || b.ty == 5) return a.ty == 5 && b.ty == 5 && a.s == b.s;
    if (a.ty == 4 || b.ty == 4) return a.ty == 4 && b.ty == 4 && a.b == b.b;
    return rose_dyn_cmp(a, b) == 0;
}

rose_table rose_table_new() { return rose_table(); }

void rose_table_inserer(rose_table &t, const rose_str &cle, const rose_dyn &v) {
    for (auto &kv : t) if (kv.first == cle) { kv.second = v; return; }   // remplace EN PLACE
    t.push_back({cle, v});
}
bool rose_table_cle_existe(const rose_table &t, const rose_str &cle) {
    for (auto &kv : t) if (kv.first == cle) return true;
    return false;
}
bool rose_table_supprimer(rose_table &t, const rose_str &cle) {
    for (size_t i = 0; i < t.size(); i++)
        if (t[i].first == cle) { t.erase(t.begin() + i); return true; }
    return false;
}
rose_int rose_table_longueur(const rose_table &t) { return (rose_int)t.size(); }
rose_dyn rose_table_get(const rose_table &t, const rose_str &cle) {
    for (auto &kv : t) if (kv.first == cle) return kv.second;
    rose_erreur("clé absente de la table");
    return rose_dyn();
}
rose_liste rose_table_cles(const rose_table &t) {
    rose_liste l; for (auto &kv : t) l.push_back(kv.first); return l;
}
rose_liste rose_table_valeurs(const rose_table &t) {
    rose_liste l; for (auto &kv : t) l.push_back(kv.second.rendu()); return l;   // les listes rendent par ecri (miroir)
}
void rose_afficher_table(const rose_table &t) {          // miroir ecri(T_TABLE) → fputs(chaine_de)
    std::string r = "[";
    for (size_t i = 0; i < t.size(); i++) {
        if (i) r += ", ";
        r += t[i].first + " => " + t[i].second.texte();
    }
    r += "]\n";
    std::fputs(r.c_str(), stdout);
}
bool rose_table_eq(const rose_table &a, const rose_table &b) {   // ordre independant (miroir)
    if (a.size() != b.size()) return false;
    for (auto &kv : a) {
        bool found = false;
        for (auto &kv2 : b)
            if (kv.first == kv2.first && rose_dyn_eq(kv.second, kv2.second)) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

rose_str rose_texte_q_ab(const rose_qd &q_in) {   // miroir chaine_de(T_RATIONNEL)
    mpq_class q = q_in;
    q.canonicalize();
    if (mpz_get_si(q.get_den().get_mpz_t()) == 1 && mpz_fits_slong_p(q.get_den().get_mpz_t()))
        return q.get_num().get_str();
    return q.get_num().get_str() + "/" + q.get_den().get_str();
}

// ── Aléatoire « vrai » : source non déterministe ──────────────────────────
static uint64_t rose_lire_octets_systeme(void) {
    uint64_t v = 0;
    // Matériel d'abord si disponible.
    if (rose_rng_materiel()) {
#if defined(__aarch64__)
        unsigned long r = 0;
        // RNDR (FEAT_RNG) : mrs ... ; NZCV indique l'échec.
        __asm__ volatile("mrs %0, s3_3_c2_c4_0" : "=r"(r));
        v = (uint64_t)r;
#endif
    }
    if (v == 0) {
        ssize_t got = getrandom(&v, sizeof v, 0);
        if (got != (ssize_t)sizeof v) {
            // Repli : mélange pid + adresse + horloge.
            v = (uint64_t)getpid() * 0x9E3779B97F4A7C15ULL
              ^ (uint64_t)(uintptr_t)&v
              ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        }
    }
    return v;
}

int rose_rng_materiel(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 0;
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Features", 0) == 0 || line.rfind("flags", 0) == 0) {
            std::istringstream ss(line);
            std::string tok;
            // saute le nom du champ
            ss >> tok;
            while (ss >> tok) {
                if (tok == "rng" || tok == "rdrand" || tok == "rdseed") { cached = 1; break; }
            }
        }
        if (cached) break;
    }
    return cached;
}

void rose_graine_systeme(void) {
    g_rng_etat = rose_lire_octets_systeme();
    rose_rng_tirer();
}

rose_int rose_hasard_systeme(rose_int n) {
    if (n <= 0) return 0;
    // Tirage non biaisé (Lemire) sur 64 bits.
    uint64_t x = rose_lire_octets_systeme();
    // Pour n <= 2^63, réduction 128-bit.
    __uint128_t m = (__uint128_t)x * (unsigned long long)n;
    uint64_t l = (uint64_t)m;
    if (l < (uint64_t)n) {
        uint64_t t = (-(uint64_t)n) % (uint64_t)n;
        while (l < t) { x = rose_lire_octets_systeme(); m = (__uint128_t)x * (unsigned long long)n; l = (uint64_t)m; }
    }
    return (rose_int)((m >> 64));
}

rose_int rose_hasard_vrai(rose_int n) { return rose_hasard_systeme(n); }

rose_ent rose_hasard_systeme(const rose_ent &n) {
    if (!n.gros()) {
        int64_t nn = n.petit();
        if (nn <= 0) rose_erreur("hasard : bornage positif attendu");
        uint64_t x = rose_lire_octets_systeme();
        if ((nn & (nn - 1)) == 0) return rose_ent((int64_t)(x & (uint64_t)(nn - 1)));
        return rose_ent((int64_t)((__uint128_t)x * (uint64_t)nn >> 64));
    }
    if (mpz_sgn(n.gd().get_mpz_t()) <= 0) rose_erreur("hasard : bornage positif attendu");
    uint64_t x = rose_lire_octets_systeme();
    mpz_class rr;
    mpz_set_ui(rr.get_mpz_t(), (unsigned long)(x >> 32));
    mpz_mul_2exp(rr.get_mpz_t(), rr.get_mpz_t(), 32);
    mpz_add_ui(rr.get_mpz_t(), rr.get_mpz_t(), (unsigned long)(x & 0xFFFFFFFFu));
    mpz_mod(rr.get_mpz_t(), rr.get_mpz_t(), n.gd().get_mpz_t());
    return rose_ent(rr);
}
rose_ent rose_hasard_vrai(const rose_ent &n) { return rose_hasard_systeme(n); }


