// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cc/mcts_player.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "cc/check.h"
#include "cc/random.h"
#include "cc/symmetries.h"

namespace minigo {

std::ostream& operator<<(std::ostream& os, const MctsPlayer::Options& options) {
  os << "name:" << options.name << " inject_noise:" << options.inject_noise
     << " soft_pick:" << options.soft_pick
     << " random_symmetry:" << options.random_symmetry
     << " resign_threshold:" << options.resign_threshold
     << " batch_size:" << options.batch_size << " komi:" << options.komi
     << " num_readouts:" << options.num_readouts
     << " seconds_per_move:" << options.seconds_per_move
     << " time_limit:" << options.time_limit
     << " decay_factor:" << options.decay_factor
     << " random_seed:" << options.random_seed;
  return os;
}

float TimeRecommendation(int move_num, float seconds_per_move, float time_limit,
                         float decay_factor) {
  // Divide by two since you only play half the moves in a game.
  int player_move_num = move_num / 2;

  // Sum of geometric series maxes out at endgame_time seconds.
  float endgame_time = seconds_per_move / (1.0f - decay_factor);

  float base_time;
  int core_moves;
  if (endgame_time > time_limit) {
    // There is so little main time that we're already in 'endgame' mode.
    base_time = time_limit * (1.0f - decay_factor);
    core_moves = 0;
  } else {
    // Leave over endgame_time seconds for the end, and play at
    // seconds_per_move for as long as possible.
    base_time = seconds_per_move;
    core_moves = (time_limit - endgame_time) / seconds_per_move;
  }

  return base_time *
         std::pow(decay_factor, std::max(player_move_num - core_moves, 0));
}

MctsPlayer::MctsPlayer(std::unique_ptr<DualNet> network, const Options& options)
    : network_(std::move(network)),
      game_root_(&dummy_stats_, {&bv_, &gv_, Color::kBlack}),
      rnd_(options.random_seed),
      options_(options) {
  options_.resign_threshold = -std::abs(options_.resign_threshold);
  // When to do deterministic move selection: 30 moves on a 19x19, 6 on 9x9.
  temperature_cutoff_ = kN * kN / 12;
  root_ = &game_root_;

  if (options_.verbose) {
    std::cerr << "MctsPlayer options: " << options_ << "\n";
    std::cerr << "Random seed used: " << rnd_.seed() << "\n";
  }

  InitializeGame({&bv_, &gv_, Color::kBlack});
}

MctsPlayer::~MctsPlayer() {
  if (options_.verbose) {
    std::cerr << "Inference history:" << std::endl;
    for (const auto& info : inferences_) {
      std::cerr << info.model << " [" << info.first_move << ", "
                << info.last_move << "]" << std::endl;
    }
  }
}

void MctsPlayer::InitializeGame(const Position& position) {
  game_root_ = {&dummy_stats_, Position(&bv_, &gv_, position)};
  root_ = &game_root_;
  game_over_ = false;
}

void MctsPlayer::NewGame() {
  game_root_ = MctsNode(&dummy_stats_, {&bv_, &gv_, Color::kBlack});
  root_ = &game_root_;
  game_over_ = false;
}

Coord MctsPlayer::SuggestMove() {
  auto start = absl::Now();

  std::array<float, kNumMoves> noise;
  if (options_.inject_noise) {
    // In order to be able to inject noise into the root node, we need to first
    // expand it. The root will always be expanded unless this is the first time
    // SuggestMove has been called for a game.
    if (!root_->is_expanded) {
      auto* first_node = root_->SelectLeaf();
      ProcessLeaves({&first_node, 1});
    }

    rnd_.Dirichlet(kDirichletAlpha, &noise);
    root_->InjectNoise(noise);
  }
  int current_readouts = root_->N();

  if (options_.seconds_per_move > 0) {
    // Use time to limit the number of reads.
    float seconds_per_move = options_.seconds_per_move;
    if (options_.time_limit > 0) {
      seconds_per_move =
          TimeRecommendation(root_->position.n(), seconds_per_move,
                             options_.time_limit, options_.decay_factor);
    }
    while (absl::Now() - start < absl::Seconds(seconds_per_move)) {
      TreeSearch(options_.batch_size);
    }
  } else {
    // Use a fixed number of reads.
    while (root_->N() < current_readouts + options_.num_readouts) {
      TreeSearch(options_.batch_size);
    }
  }
  int num_readouts = root_->N() - current_readouts;
  auto elapsed = absl::Now() - start;
  elapsed = elapsed * 100 / num_readouts;
  if (options_.verbose) {
    std::cerr << "Milliseconds per 100 reads: "
              << absl::ToInt64Milliseconds(elapsed) << "ms"
              << " over " << num_readouts
              << " readouts (batched: " << options_.batch_size << ")"
              << std::endl;
  }

  if (ShouldResign()) {
    return Coord::kResign;
  }

  return PickMove();
}

Coord MctsPlayer::PickMove() {
  if (!options_.soft_pick || root_->position.n() >= temperature_cutoff_) {
    // Choose the most visited node.
    Coord c = ArgMax(root_->edges, MctsNode::CmpN);
    if (options_.verbose) {
      std::cerr << "Picked arg_max " << c << "\n";
    }
    return c;
  }

  // Select from the first kN * kN moves (instead of kNumMoves) to avoid
  // randomly choosing to pass early on in the game.
  std::array<float, kN * kN> cdf;

  cdf[0] = root_->child_N(0);
  for (size_t i = 1; i < cdf.size(); ++i) {
    cdf[i] = cdf[i - 1] + root_->child_N(i);
  }
  float norm = 1 / cdf[cdf.size() - 1];
  for (size_t i = 0; i < cdf.size(); ++i) {
    cdf[i] *= norm;
  }
  float e = rnd_();
  Coord c = SearchSorted(cdf, e);
  if (options_.verbose) {
    std::cerr << "Picked rnd(" << e << ") " << c << "\n";
  }
  MG_DCHECK(root_->child_N(c) != 0);
  return c;
}

absl::Span<MctsNode* const> MctsPlayer::TreeSearch(int batch_size) {
  int max_iterations = batch_size * 2;

  leaves_.clear();
  for (int i = 0; i < max_iterations; ++i) {
    auto* leaf = root_->SelectLeaf();
    if (leaf == nullptr) {
      continue;
    }
    if (leaf->position.is_game_over() ||
        leaf->position.n() >= kMaxSearchDepth) {
      float value = leaf->position.CalculateScore(options_.komi) > 0 ? 1 : -1;
      leaf->IncorporateEndGameResult(value, root_);
    } else {
      leaf->AddVirtualLoss(root_);
      leaves_.push_back(leaf);
      if (static_cast<int>(leaves_.size()) == batch_size) {
        break;
      }
    }
  }

  if (!leaves_.empty()) {
    ProcessLeaves(absl::MakeSpan(leaves_));
    for (auto* leaf : leaves_) {
      leaf->RevertVirtualLoss(root_);
    }
  }

  return absl::MakeConstSpan(leaves_);
}

bool MctsPlayer::ShouldResign() const {
  return root_->Q_perspective() < options_.resign_threshold;
}

void MctsPlayer::PlayMove(Coord c) {
  if (game_over_) {
    std::cerr << "ERROR: can't play move " << c << ", game is over"
              << std::endl;
    return;
  }

  // Handle resignations.
  if (c == Coord::kResign) {
    if (root_->position.to_play() == Color::kBlack) {
      result_ = -1;
      result_string_ = "W+R";
    } else {
      result_ = 1;
      result_string_ = "B+R";
    }
    game_over_ = true;
    return;
  }

  PushHistory(c);

  root_ = root_->MaybeAddChild(c);
  // Don't need to keep the parent's children around anymore because we'll
  // never revisit them.
  root_->parent->PruneChildren(c);

  if (options_.verbose) {
    std::cerr << name() << " Q: " << std::setw(8) << std::setprecision(5)
              << root_->Q() << "\n";
    std::cerr << "Played >>" << c << std::endl;
  }

  // Handle consecutive passing.
  if (root_->position.is_game_over() ||
      root_->position.n() >= kMaxSearchDepth) {
    float score = root_->position.CalculateScore(options_.komi);
    result_string_ = FormatScore(score);
    result_ = score < 0 ? -1 : score > 0 ? 1 : 0;
    game_over_ = true;
  }
}

std::string MctsPlayer::FormatScore(float score) const {
  std::ostringstream oss;
  oss << std::fixed;
  if (score > 0) {
    oss << "B+" << std::setprecision(1) << score;
  } else {
    oss << "W+" << std::setprecision(1) << -score;
  }
  return oss.str();
}

void MctsPlayer::PushHistory(Coord c) {
  history_.emplace_back();
  History& history = history_.back();
  history.c = c;
  history.comment = root_->Describe();
  history.node = root_;

  if (!inferences_.empty()) {
    // Record which model(s) were used when running tree search for this move.
    std::vector<std::string> models;
    for (auto it = inferences_.rbegin(); it != inferences_.rend(); ++it) {
      if (it->last_move < root_->position.n()) {
        break;
      }
      models.push_back(it->model);
    }
    std::reverse(models.begin(), models.end());
    auto model_comment = absl::StrCat("models:", absl::StrJoin(models, ","));
    history.comment = absl::StrCat(model_comment, "\n", history.comment);
    if (options_.verbose) {
      std::cerr << model_comment << std::endl;
    }
  }

  // Convert child visit counts to a probability distribution, pi.
  // For moves before the temperature cutoff, exponentiate the probabilities by
  // a temperature slightly larger than unity to encourage diversity in early
  // play and hopefully to move away from 3-3s.
  if (root_->position.n() < temperature_cutoff_) {
    // Squash counts before normalizing.
    for (int i = 0; i < kNumMoves; ++i) {
      history.search_pi[i] = std::pow(root_->child_N(i), 0.98);
    }
  } else {
    for (int i = 0; i < kNumMoves; ++i) {
      history.search_pi[i] = root_->child_N(i);
    }
  }
  // Normalize counts.
  float sum = 0;
  for (int i = 0; i < kNumMoves; ++i) {
    sum += history.search_pi[i];
  }
  for (int i = 0; i < kNumMoves; ++i) {
    history.search_pi[i] /= sum;
  }
}

void MctsPlayer::ProcessLeaves(absl::Span<MctsNode*> leaves) {
  // Select symmetry operations to apply.
  symmetries_used_.clear();
  if (options_.random_symmetry) {
    symmetries_used_.reserve(leaves.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
      symmetries_used_.push_back(static_cast<symmetry::Symmetry>(
          rnd_.UniformInt(0, symmetry::kNumSymmetries - 1)));
    }
  } else {
    symmetries_used_.resize(leaves.size(), symmetry::kIdentity);
  }

  // Build input features for each leaf, applying random symmetries if
  // requested.
  DualNet::BoardFeatures raw_features;
  features_.resize(leaves.size());
  for (size_t i = 0; i < leaves.size(); ++i) {
    leaves[i]->GetMoveHistory(DualNet::kMoveHistory, &recent_positions_);
    DualNet::SetFeatures(recent_positions_, leaves[i]->position.to_play(),
                         &raw_features);
    symmetry::ApplySymmetry<float, kN, DualNet::kNumStoneFeatures>(
        symmetries_used_[i], raw_features.data(), features_[i].data());
  }

  // Run inference.
  outputs_.resize(leaves.size());
  network_->RunMany(features_, absl::MakeSpan(outputs_), &model_);

  // Record some information about the inference.
  if (!model_.empty()) {
    if (inferences_.empty() || model_ != inferences_.back().model) {
      inferences_.emplace_back(model_, root_->position.n());
    }
    inferences_.back().last_move = root_->position.n();
    inferences_.back().total_count += leaves.size();
  }

  // Incorporate the inference outputs back into tree search, undoing any
  // previously applied random symmetries.
  std::array<float, kNumMoves> raw_policy;
  for (size_t i = 0; i < leaves.size(); ++i) {
    MctsNode* leaf = leaves[i];
    const auto& output = outputs_[i];
    symmetry::ApplySymmetry<float, kN, 1>(
        symmetry::Inverse(symmetries_used_[i]), output.policy.data(),
        raw_policy.data());
    raw_policy[Coord::kPass] = output.policy[Coord::kPass];
    leaf->IncorporateResults(raw_policy, output.value, root_);
  }
}

}  // namespace minigo
