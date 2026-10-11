#ifndef GOMOKU_POSITION_JSON_H
#define GOMOKU_POSITION_JSON_H

#include "../core/board.h"
#include "../core/position_setup.h"
#include <nlohmann/json.hpp>
#include <string>

namespace gomoku {

// The canonical setup document, version 1. Exactly one of board / bitboard /
// moves:
//
//   {
//     "version": 1,
//     "encoding": { "black": 1, "white": 2, "empty": 0 },  // optional
//     "board": [[0, 0, ...], ...]                          // 15 rows x 15 values
//   |  "board": "0,0,...\n0,1,..."                         // or a text grid
//   |  "bitboard": { "black": ["0x0","0x0","0x0","0x0"],   // 4 words, LSB first
//   |                "white": [...] }
//   |  "moves": [[7,7],[8,8], ...], "first": "black"       // alternating colours
//   }
//
// `encoding` states the token mapping instead of leaving it to detection: a
// number is one of the engine's own cell values (0/1/2) and a string is a grid
// token, so {"black":"B","white":"W","empty":"."} and {"black":1,"white":2,
// "empty":0} are both accepted. Automated callers (training, replay, tests)
// always state it, which is what makes their templates mutually readable without
// a human confirming the reading; the UI states it too, as the second step the
// player confirms.
using json = nlohmann::json;

SetupResult position_from_json(const json& doc);

// Text in either shape: a JSON document when it begins with `{` or `[`,
// otherwise a plain-text grid. This is what a file upload and a pasted board
// both land on.
SetupResult position_from_text(const std::string& text);

// Read a file through position_from_text. CSV, txt and json all work.
bool load_position_file(const std::string& path, SetupResult& out, std::string& error);

// Emit the canonical document for a board, for the UI's export button and for
// test fixtures.
std::string position_to_json(const Board& board);

}

#endif
