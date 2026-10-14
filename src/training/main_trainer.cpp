#include "trainer.h"
#include "params.h"
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gomoku;

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "Self-play training for the threat coefficients used by the engine.\n"
        "Each round optimises a candidate set, plays it against the current\n"
        "champion over the table with the colours swapped, promotes it only if it\n"
        "clears --accept-score, and writes a checkpoint so the run can resume.\n"
        "\n"
        "  --method genetic|hill       Optimiser used inside every round (genetic)\n"
        "  --rounds N                  Rounds to run (3). With --resume these are\n"
        "                              additional rounds on top of the checkpoint.\n"
        "  --population N              Genetic population size (8)\n"
        "  --generations N             Genetic generations per round (4)\n"
        "  --stagnation N              Generations without improvement that end\n"
        "                              a genetic round; hill climbing uses 3x\n"
        "                              this as its probe budget (3)\n"
        "  --hill-iterations N         Hill-climbing neighbour probes per round;\n"
        "                              identical neighbours are skipped, and the\n"
        "                              champion is scored once on top: at most\n"
        "                              N+1 evaluations per round (12)\n"
        "  --depth N                   Search depth during training games (4)\n"
        "  --timeout-ms N              Per-move thinking budget in ms (400)\n"
        "  --max-moves N               Ply cap for a training game (60)\n"
        "  --fitness-games N           Games used to score one candidate (2)\n"
        "  --verification-games N      Games a candidate must beat the champion\n"
        "                              over before promotion (8)\n"
        "  --accept-score X            Promotion threshold, >0.5 and <=1.0 (0.56)\n"
        "  --data-games N              Self-play games kept as training data (4)\n"
        "  --threads N                 Parallel data-collection workers (2)\n"
        "  --no-data                   Collect no self-play data (search only)\n"
        "  --no-resign                 Never resign during training games\n"
        "  --seed N                    RNG seed (20261011)\n"
        "  --params PATH               Starting coefficients (built-in defaults)\n"
        "                              (not together with --resume)\n"
        "  --out PATH                  Where the champion is written\n"
        "                              (data/training/best_params.json)\n"
        "  --records PATH              Append played games as JSONL\n"
        "  --samples PATH              Append training samples as JSONL\n"
        "  --checkpoint PATH           Round-by-round resume state\n"
        "                              (data/training/checkpoint.json)\n"
        "  --resume PATH               Continue from this checkpoint, and write the\n"
        "                              rest of the run back to it; when --checkpoint\n"
        "                              is given too, the two must name one file\n"
        "                              (not together with --params)\n"
        "  --quiet                     Only print the final result\n"
        "  --help                      Show this text\n";
}

std::string require_value(const char* flag, int& index, int argc, char** argv) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") + flag);
    }
    const std::string value = argv[++index];
    // An empty value is usually an unexpanded shell variable, and for a path it is
    // destructive: --resume "" skips the resume, then overwrites the default checkpoint.
    if (value.empty()) {
        throw std::invalid_argument(std::string("Invalid ") + flag + " (must not be empty)");
    }
    return value;
}

// A count of zero for anything but --rounds is not a smaller run, it is a run
// that silently does nothing (no verification games means no candidate is ever
// promoted), so the bounds are checked here rather than at each call site.
int require_int(const char* flag, int& index, int argc, char** argv, int minimum = 1) {
    const std::string text = require_value(flag, index, argc, argv);
    size_t used = 0;
    const int value = std::stoi(text, &used);
    if (used != text.size()) {
        throw std::invalid_argument(std::string("invalid integer for ") + flag + ": " + text);
    }
    if (value < minimum) {
        throw std::invalid_argument(std::string(flag) + " must be >= " +
                                    std::to_string(minimum) + ", got " + std::to_string(value));
    }
    return value;
}

double require_double(const char* flag, int& index, int argc, char** argv) {
    const std::string text = require_value(flag, index, argc, argv);
    size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size()) {
        throw std::invalid_argument(std::string("invalid number for ") + flag + ": " + text);
    }
    // stod parses "nan" and "inf" as numbers with used==size, and a NaN passes
    // every range guard below it, so the threshold has to be checked here.
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string("invalid number for ") + flag + ": " + text);
    }
    return value;
}

uint64_t require_seed(const char* flag, int& index, int argc, char** argv) {
    const std::string text = require_value(flag, index, argc, argv);
    // stoull does not reject a sign: "-1" wraps to 18446744073709551615 and the
    // caller believes that is the seed. A seed is digits or nothing.
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument(std::string("invalid seed for ") + flag + ": " + text);
    }
    return static_cast<uint64_t>(std::stoull(text));
}

// A training run writes several artefacts, and the default paths live in a
// directory that only exists after a build. Creating it here keeps a bare
// `./gomoku_train` from failing on the first promotion.
void ensure_parent_directory(const std::string& path) {
    if (path.empty()) return;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
}

void print_coefficients(const ThreatCoefficients& coeffs) {
    std::cout << "champion coefficients:\n"
              << "  live_four        = " << coeffs.live_four << "\n"
              << "  rush_four        = " << coeffs.rush_four << "\n"
              << "  live_three       = " << coeffs.live_three << "\n"
              << "  sleepy_three     = " << coeffs.sleepy_three << "\n"
              << "  live_two         = " << coeffs.live_two << "\n"
              << "  sleepy_two       = " << coeffs.sleepy_two << "\n"
              << "  double_four      = " << coeffs.double_four << "\n"
              << "  double_three     = " << coeffs.double_three << "\n"
              << "  four_three       = " << coeffs.four_three << "\n"
              << "  position_scale   = " << coeffs.position_weight_scale << "\n"
              << "  five             = " << coeffs.five << " (never tuned)\n";
}

}

int main(int argc, char** argv) {
    TrainingConfig config;
    std::string base_params;
    std::string resume_path;
    // Both defaults live under data/training/, which .gitignore already excludes:
    // a training run should never dirty the repository.
    std::string out_path = "data/training/best_params.json";
    std::string checkpoint_path = "data/training/checkpoint.json";
    bool checkpoint_given = false;
    bool quiet = false;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--help" || flag == "-h") {
                print_usage(argv[0]);
                return 0;
            } else if (flag == "--method") {
                const std::string method = require_value("--method", i, argc, argv);
                if (method == "genetic") {
                    config.optimizer = OptimizerKind::GENETIC;
                } else if (method == "hill" || method == "hill-climbing") {
                    config.optimizer = OptimizerKind::HILL_CLIMBING;
                } else {
                    throw std::invalid_argument("unknown --method " + method +
                                                " (use genetic or hill)");
                }
            } else if (flag == "--rounds") {
                // Only negatives fail here; a zero round count parses and is
                // refused below, where the run would train nothing.
                config.rounds = require_int("--rounds", i, argc, argv, 0);
            } else if (flag == "--population") {
                config.population_size = require_int("--population", i, argc, argv);
            } else if (flag == "--generations") {
                config.generations = require_int("--generations", i, argc, argv);
            } else if (flag == "--stagnation") {
                config.stagnation_generations = require_int("--stagnation", i, argc, argv);
            } else if (flag == "--hill-iterations") {
                config.hill_climbing_iterations = require_int("--hill-iterations", i, argc, argv);
            } else if (flag == "--depth") {
                config.search_depth = require_int("--depth", i, argc, argv);
            } else if (flag == "--timeout-ms") {
                // The bounds are the guard below's business (--timeout-ms at least
                // 1, --max-moves at least 2), so this parses any integer written.
                config.timeout_ms = require_int("--timeout-ms", i, argc, argv, INT_MIN);
            } else if (flag == "--max-moves") {
                config.max_moves = require_int("--max-moves", i, argc, argv, INT_MIN);
            } else if (flag == "--fitness-games") {
                config.fitness_games = require_int("--fitness-games", i, argc, argv);
            } else if (flag == "--verification-games") {
                config.verification_games = require_int("--verification-games", i, argc, argv);
            } else if (flag == "--accept-score") {
                config.accept_score = require_double("--accept-score", i, argc, argv);
            } else if (flag == "--data-games") {
                config.data_games = require_int("--data-games", i, argc, argv);
            } else if (flag == "--threads") {
                config.data_threads = require_int("--threads", i, argc, argv);
            } else if (flag == "--no-data") {
                config.collect_data = false;
            } else if (flag == "--no-resign") {
                config.resign_enabled = false;
            } else if (flag == "--seed") {
                config.seed = require_seed("--seed", i, argc, argv);
            } else if (flag == "--params") {
                base_params = require_value("--params", i, argc, argv);
            } else if (flag == "--out") {
                out_path = require_value("--out", i, argc, argv);
            } else if (flag == "--records") {
                config.records_path = require_value("--records", i, argc, argv);
            } else if (flag == "--samples") {
                config.samples_path = require_value("--samples", i, argc, argv);
            } else if (flag == "--checkpoint") {
                checkpoint_path = require_value("--checkpoint", i, argc, argv);
                checkpoint_given = true;
            } else if (flag == "--resume") {
                resume_path = require_value("--resume", i, argc, argv);
            } else if (flag == "--quiet") {
                quiet = true;
            } else {
                throw std::invalid_argument("unknown option " + flag);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\nTry --help for the option list.\n";
        return 1;
    }

    if (config.rounds <= 0) {
        std::cerr << "Error: --rounds must be at least 1" << std::endl;
        return 1;
    }
    // 0.5 is a draw score, so anything at or below it would promote a candidate
    // that merely survived.
    if (config.accept_score <= 0.5 || config.accept_score > 1.0) {
        std::cerr << "Error: --accept-score must be greater than 0.5 and at most 1.0"
                  << std::endl;
        return 1;
    }
    if (config.search_depth < 1 || config.timeout_ms < 1 || config.max_moves < 2) {
        std::cerr << "Error: --depth, --timeout-ms and --max-moves must be positive"
                  << " (--max-moves at least 2)" << std::endl;
        return 1;
    }

    // --resume names the file being read, so it is also the file being written
    // back: without this, --resume ck.json without --checkpoint kept writing the
    // default file and ck.json never advanced, so the next resume replayed the
    // same rounds. When both are given they must agree - silently picking one
    // would leave the other stale.
    if (!resume_path.empty()) {
        if (!checkpoint_given) {
            checkpoint_path = resume_path;
        } else if (checkpoint_path != resume_path) {
            std::cerr << "Error: --checkpoint " << checkpoint_path << " and --resume "
                      << resume_path << " must name the same file when both are given"
                      << std::endl;
            return 1;
        }
    }
    // Both name the starting point for this round, and a resume replaces what was
    // loaded: keeping both would read and announce a file the run never uses.
    if (!base_params.empty() && !resume_path.empty()) {
        std::cerr << "Error: --params " << base_params << " and --resume " << resume_path
                  << " cannot both be given (each names the starting point for this round)"
                  << std::endl;
        return 1;
    }

    config.params_path = out_path;
    config.checkpoint_path = checkpoint_path;

    ThreatCoefficients base;
    if (!base_params.empty()) {
        std::string error;
        if (!CoefficientVector::load(base_params, base, error)) {
            std::cerr << "Error: cannot use --params " << base_params << ": " << error << std::endl;
            return 1;
        }
        if (!quiet) std::cout << "Starting from " << base_params << std::endl;
    }

    Trainer trainer(base, config);
    if (!resume_path.empty()) {
        std::string error;
        if (!trainer.load_checkpoint(resume_path, error)) {
            std::cerr << "Error: cannot resume from " << resume_path << ": " << error << std::endl;
            return 1;
        }
        if (!quiet) {
            std::cout << "Resumed " << resume_path << " (" << trainer.status_text() << ")"
                      << std::endl;
        }
    }
    if (!quiet) trainer.set_log_callback([](const std::string& line) {
        std::cout << line << std::endl;
    });

    for (const auto& path : {out_path, checkpoint_path, config.records_path, config.samples_path})
        ensure_parent_directory(path);

    std::vector<TrainerRound> history = trainer.run(config.rounds);

    std::string error;
    if (!CoefficientVector::save(out_path, trainer.champion(), error)) {
        std::cerr << "Error: cannot write champion parameters: " << error << std::endl;
        return 1;
    }

    std::cout << "\nTraining finished: " << trainer.status_text() << "\n";
    for (const auto& round : history) {
        std::cout << "  round " << round.round << ": score " << round.verification_score
                  << " -> " << (round.accepted ? "promoted" : "rejected") << "\n";
    }

    std::cout << "\nRatings:\n";
    for (const auto& player : trainer.ratings().standings()) {
        std::cout << "  " << player.name << "  " << player.elo << "  (" << player.wins << "-"
                  << player.losses << "-" << player.draws << ")\n";
    }

    print_coefficients(trainer.champion());
    std::cout << "\nChampion written to " << out_path << std::endl;
    std::cout << "Start the server with: ./gomoku_server --params " << out_path << std::endl;
    return 0;
}
