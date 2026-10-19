#ifndef GOMOKU_SPRT_H
#define GOMOKU_SPRT_H

#include <cmath>

namespace gomoku {

// Design doc 2.8 #3: a match is a sequential test, not a fixed number of games.
// Each game moves the log-likelihood ratio between "the candidate wins a share
// p1 of games" and "it wins half of them" by
//
//     s * ln(p1 / 0.5) + (1 - s) * ln((1 - p1) / 0.5)
//
// for the candidate's score s, so a win and a loss are the two terms and a draw
// scores their average - the standard model without separate draw odds, and the
// reason a drawn game is evidence *against* a large p1 rather than neutral: two
// sides a stated Elo apart do not draw as often as two equal sides do.
//
// p1 comes from the Elo formula at `effect_elo`, which makes the alternative
// hypothesis a per-game win rate, not the per-match number EloSystem prints:
// 300 is "wins 84.9% of games", and the bound it has to reach is +/-ln(19) =
// 2.9444 at the 5% risks below. The ceiling a match is asked for then decides
// what can close at all, and that arithmetic is why the default is 300 rather
// than a textbook 50: at 50 Elo every term is under 0.16 in magnitude, so the
// shipped 8-game ceiling tops out at 1.23 and the test never closes; at 250 a
// clean sweep still only reaches the bound on its last pair. At 300 a 0-4 record
// is -4.79 (reject after 4 games) and a 6-0 record is +3.18 (accept after 6),
// while a match that splits every pair - which is what a colour-decided board
// produces - drifts to -2.67 and stays open for --accept-score to decide.
struct SprtConfig {
    // Elo difference the test measures against. Zero turns the sequential test
    // off and every match plays its fixed pair count.
    int effect_elo = 300;
    // Risk of promoting a candidate that is not better (upper bound is
    // ln((1 - alpha) / alpha)).
    double alpha = 0.05;
    // Risk of keeping the champion against one that is better.
    double beta = 0.05;
};

enum class SprtVerdict {
    OPEN,     // inside both bounds: the ceiling was reached without a decision
    ACCEPT,
    REJECT,
};

inline const char* sprt_verdict_name(SprtVerdict verdict) {
    switch (verdict) {
        case SprtVerdict::ACCEPT: return "accept";
        case SprtVerdict::REJECT: return "reject";
        case SprtVerdict::OPEN: break;
    }
    return "open";
}

// A match plays colour-swapped PAIRS, so the test only decides between pairs:
// one game decides nothing about strength but who moved first, and README's
// 颜色通吃 record is a match where every single-game lead was exactly that.
class SprtTest {
public:
    explicit SprtTest(const SprtConfig& config)
        : enabled_(config.effect_elo > 0),
          upper_(std::log((1.0 - config.alpha) / config.alpha)),
          lower_(-std::log((1.0 - config.beta) / config.beta)) {
        if (!enabled_) return;
        // A win is scored as p1, a loss as 1 - p1, so the two terms are the only
        // numbers the per-game update needs. p1 is clamped off 1.0 because an Elo
        // this side of ten thousand rounds to exactly 1.0 in a double, and ln(0)
        // would hand the match a NaN ratio no bound can compare against.
        const double odds = std::pow(10.0, -static_cast<double>(config.effect_elo) / 400.0);
        const double raw = 1.0 / (1.0 + odds);
        const double p1 = raw < 1.0 - 1e-9 ? raw : 1.0 - 1e-9;
        win_term_ = std::log(p1 / 0.5);
        loss_term_ = std::log((1.0 - p1) / 0.5);
    }

    // One game's score from the candidate's side: 0, 0.5 or 1.
    void add_game(double score) {
        ++games_;
        if (!enabled_) return;
        llr_ += score * win_term_ + (1.0 - score) * loss_term_;
    }

    // Called after both colours have been played. The verdict, once given, is not
    // revisited, so `stop_requested` stays true for the rest of the match.
    void end_pair() {
        if (decided_ || !enabled_) return;
        if (llr_ >= upper_) verdict_ = SprtVerdict::ACCEPT;
        else if (llr_ <= lower_) verdict_ = SprtVerdict::REJECT;
        else return;
        decided_ = true;
    }

    bool stop_requested() const { return decided_; }

    SprtVerdict verdict() const { return verdict_; }
    double llr() const { return llr_; }
    int games() const { return games_; }
    double upper_bound() const { return upper_; }
    double lower_bound() const { return lower_; }

private:
    bool enabled_ = false;
    double win_term_ = 0.0;
    double loss_term_ = 0.0;
    double upper_ = 0.0;
    double lower_ = 0.0;
    double llr_ = 0.0;
    int games_ = 0;
    bool decided_ = false;
    SprtVerdict verdict_ = SprtVerdict::OPEN;
};

}

#endif
