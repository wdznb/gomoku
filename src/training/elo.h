#ifndef GOMOKU_ELO_H
#define GOMOKU_ELO_H

#include <string>
#include <vector>

namespace gomoku {

// Rating bookkeeping for parameter versions. Deliberately free of any engine
// dependency: it only turns game results into numbers, so it can be tested and
// reused without playing anything.
class EloSystem {
public:
    struct Player {
        std::string name;
        double elo = 1500.0;
        int wins = 0;
        int losses = 0;
        int draws = 0;
        int games = 0;
    };

    explicit EloSystem(double initial_elo = 1500.0, double base_k = 32.0);

    // Returns the id of `name`, creating it at the configured initial rating
    // when unseen. Registering a name twice is an alias lookup, not a duplicate
    // player.
    int add_player(const std::string& name);
    int add_player(const std::string& name, double elo);
    int player_id(const std::string& name) const;
    bool has_player(const std::string& name) const;
    const Player& player(int id) const;
    const std::vector<Player>& players() const { return players_; }

    static double expected_score(double rating_a, double rating_b);

    // K shrinks as a player accumulates games, so early noisy results move the
    // rating a lot while settled ratings stop swinging.
    double k_factor(const Player& subject) const;

    // score_a is what A actually scored against B: 1 win, 0.5 draw, 0 loss.
    void record_result(int id_a, int id_b, double score_a);

    std::vector<Player> standings() const;

    std::string to_json() const;
    bool load_json(const std::string& text, std::string& error);
    bool save(const std::string& path, std::string& error) const;
    bool load(const std::string& path, std::string& error);

private:
    double initial_elo_;
    double base_k_;
    std::vector<Player> players_;
};

}

#endif
