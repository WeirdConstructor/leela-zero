/*
    This file is part of Leela Zero.
    Copyright (C) 2017-2018 Gian-Carlo Pascutto

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include "UCTNode.h"
#include "FastBoard.h"
#include "FastState.h"
#include "GTP.h"
#include "GameState.h"
#include "Network.h"
#include "Utils.h"

using namespace Utils;

UCTNode::UCTNode(int vertex, float policy) : m_move(vertex), m_policy(policy) {
}

bool UCTNode::first_visit() const {
    return m_visits == 0;
}

bool UCTNode::create_children(Network & network,
                              std::atomic<int>& nodecount,
                              GameState& state,
                              float& eval,
                              float min_psa_ratio) {
    // no successors in final state
    if (state.get_passes() >= 2) {
        return false;
    }

    // acquire the lock
    if (!acquire_expanding()) {
        return false;
    }

    // can we actually expand?
    if (!expandable(min_psa_ratio)) {
        expand_done();
        return false;
    }

    const auto raw_netlist = network.get_output(
        &state, Network::Ensemble::RANDOM_SYMMETRY);

    // DCNN returns winrate as side to move
    m_net_eval = raw_netlist.winrate;
    const auto to_move = state.board.get_to_move();
    // our search functions evaluate from black's point of view
    if (state.board.white_to_move()) {
        m_net_eval = 1.0f - m_net_eval;
    }
    eval = m_net_eval;

    std::vector<Network::PolicyVertexPair> nodelist;

    auto legal_sum = 0.0f;
    for (auto i = 0; i < NUM_INTERSECTIONS; i++) {
        const auto x = i % BOARD_SIZE;
        const auto y = i / BOARD_SIZE;
        const auto vertex = state.board.get_vertex(x, y);
        if (state.is_move_legal(to_move, vertex)) {
            nodelist.emplace_back(raw_netlist.policy[i], vertex);
            legal_sum += raw_netlist.policy[i];
        }
    }
    nodelist.emplace_back(raw_netlist.policy_pass, FastBoard::PASS);
    legal_sum += raw_netlist.policy_pass;

    if (legal_sum > std::numeric_limits<float>::min()) {
        // re-normalize after removing illegal moves.
        for (auto& node : nodelist) {
            node.first /= legal_sum;
        }
    } else {
        // This can happen with new randomized nets.
        auto uniform_prob = 1.0f / nodelist.size();
        for (auto& node : nodelist) {
            node.first = uniform_prob;
        }
    }

    link_nodelist(nodecount, nodelist, min_psa_ratio);
    expand_done();
    return true;
}

void UCTNode::link_nodelist(std::atomic<int>& nodecount,
                            std::vector<Network::PolicyVertexPair>& nodelist,
                            float min_psa_ratio) {
    assert(min_psa_ratio < m_min_psa_ratio_children);

    if (nodelist.empty()) {
        return;
    }

    // Use best to worst order, so highest go first
    std::stable_sort(rbegin(nodelist), rend(nodelist));

    const auto max_psa = nodelist[0].first;
    const auto old_min_psa = max_psa * m_min_psa_ratio_children;
    const auto new_min_psa = max_psa * min_psa_ratio;
    if (new_min_psa > 0.0f) {
        m_children.reserve(
            std::count_if(cbegin(nodelist), cend(nodelist),
                [=](const auto& node) { return node.first >= new_min_psa; }
            )
        );
    } else {
        m_children.reserve(nodelist.size());
    }

    auto skipped_children = false;
    for (const auto& node : nodelist) {
        if (node.first < new_min_psa) {
            skipped_children = true;
        } else if (node.first < old_min_psa) {
            m_children.emplace_back(node.second, node.first);
            ++nodecount;
        }
    }

    m_min_psa_ratio_children = skipped_children ? min_psa_ratio : 0.0f;
}

const std::vector<UCTNodePointer>& UCTNode::get_children() const {
    return m_children;
}


int UCTNode::get_move() const {
    return m_move;
}

void UCTNode::virtual_loss() {
    m_virtual_loss += VIRTUAL_LOSS_COUNT;
}

void UCTNode::virtual_loss_undo() {
    m_virtual_loss -= VIRTUAL_LOSS_COUNT;
}

void UCTNode::update(float eval) {
    m_visits++;
    accumulate_eval(eval);
}

bool UCTNode::has_children() const {
    return m_min_psa_ratio_children <= 1.0f;
}

bool UCTNode::expandable(const float min_psa_ratio) const {
#ifndef NDEBUG
    if (m_min_psa_ratio_children == 0.0f) {
        // If we figured out that we are fully expandable
        // it is impossible that we stay in INITIAL state.
        assert(m_expand_state.load() != ExpandState::INITIAL);
    }
#endif
    return min_psa_ratio < m_min_psa_ratio_children;
}

float UCTNode::get_policy() const {
    return m_policy;
}

void UCTNode::set_policy(float policy) {
    m_policy = policy;
}

int UCTNode::get_visits() const {
    return m_visits;
}

float UCTNode::get_raw_eval(int tomove, int virtual_loss) const {
    auto visits = get_visits() + virtual_loss;
    assert(visits > 0);
    auto blackeval = get_blackevals();
    if (tomove == FastBoard::WHITE) {
        blackeval += static_cast<double>(virtual_loss);
    }
    auto eval = static_cast<float>(blackeval / double(visits));
    if (tomove == FastBoard::WHITE) {
        eval = 1.0f - eval;
    }
    return eval;
}

float UCTNode::get_eval(int tomove) const {
    // Due to the use of atomic updates and virtual losses, it is
    // possible for the visit count to change underneath us. Make sure
    // to return a consistent result to the caller by caching the values.
    return get_raw_eval(tomove, m_virtual_loss);
}

float UCTNode::get_net_eval(int tomove) const {
    if (tomove == FastBoard::WHITE) {
        return 1.0f - m_net_eval;
    }
    return m_net_eval;
}

double UCTNode::get_blackevals() const {
    return m_blackevals;
}

void UCTNode::accumulate_eval(float eval) {
    atomic_add(m_blackevals, double(eval));
}

float UCTNode::get_visit_search_width() {
	return visit_search_width;
}

float UCTNode::get_puct_search_width() {
	return puct_search_width;
}

void UCTNode::widen_search() {
	// 0.558 multiplier = 10 increments from minimum width (0.003) to maximum width (1.0)
	visit_search_width = (0.558 * visit_search_width); // Smaller "visit_search_width" values cause the search to WIDEN
	puct_search_width = (1.11 * puct_search_width); // Larger "puct_search_width" values cause the search to WIDEN

	if (visit_search_width < 0.010) { // ORIGINALLY = 0.003 when used with the original 0.558 multiplier above
		visit_search_width = 0.010; // Numbers smaller than (1 / 362) = 0.00276 are theoretically meaningless, but I'll clamp at 100x less than that for now just in case.
		// Update: 0.0000276 crashed leelaz.exe, so I will clamp at 0.00278 which is slightly higher than theoretical minimum.
		// Update2: 0.00278 also crashed, so I'll try clamping at 0.003 instead.
		// Update3: This will be fixed someday later by better "if statement" handling inside uct_select_child.
	}
	
	if (puct_search_width < 1.0) {
		puct_search_width = 1.0; // Numbers smaller than 1.0 are unhelpful. Clamp to min policy width of of 1.0, which should be identical to traditional LZ search.
	}
}
void UCTNode::narrow_search() {
	// 1.788 multiplier = 10 increments from maximum width (1.0) to minimum width (0.003)

	visit_search_width = (1.788 * visit_search_width); // Larger visit_search_width values cause search to NARROW
	puct_search_width = (0.9 * puct_search_width); // Smaller "puct_search_width" values cause search to NARROW

	if (visit_search_width >= 1.0) {
		visit_search_width = 1.0; // Numbers larger than 1.0 are meaningless. Clamp to max narrowness of 1.0, which should be identical to traditional LZ search.
	}

	if (puct_search_width <= 0.010) {
		puct_search_width = 0.010; // This clamps at a 100x increase in puct. Requires testing, especially since the current implementation is also widening the search using "visit_search_width" at the same time.
	}
}

UCTNode* UCTNode::uct_select_child(int color, bool is_root) {
    wait_expanded();

    // Count parentvisits manually to avoid issues with transpositions.
    auto total_visited_policy = 0.0f;
    auto parentvisits = size_t{0};
    for (const auto& child : m_children) {
        if (child.valid()) {
            parentvisits += child.get_visits();
            if (child.get_visits() > 0) {
                total_visited_policy += child.get_policy();
            }
        }
    }

    auto numerator = std::sqrt(double(parentvisits));
    auto fpu_reduction = 0.0f;
    // Lower the expected eval for moves that are likely not the best.
    // Do not do this if we have introduced noise at this node exactly
    // to explore more.
    if (!is_root || !cfg_noise) {
        fpu_reduction = cfg_fpu_reduction * std::sqrt(total_visited_policy);
    }
    // Estimated eval for unknown nodes = original parent NN eval - reduction
    auto fpu_eval = get_net_eval(color) - fpu_reduction;

    auto best = static_cast<UCTNodePointer*>(nullptr);
    auto best_value = std::numeric_limits<double>::lowest();

	int legal_root_move_count = 1; // Initialized
	if (is_root) {
		legal_root_move_count = static_cast<int>(m_children.size()); // This counts the number of valid, playable intersections at the root node.
	}

    for (auto& child : m_children) {
        if (!child.active()) {
            continue;
        }

        auto winrate = fpu_eval;
        if (child.is_inflated() && child->m_expand_state.load() == ExpandState::EXPANDING) {
            // Someone else is expanding this node, never select it
            // if we can avoid so, because we'd block on it.
            winrate = -1.0f - fpu_reduction;
        } else if (child.get_visits() > 0) {
            winrate = child.get_eval(color);
        }

		float visit_search_width = get_visit_search_width();
		float puct_search_width = get_puct_search_width();
		int int_m_visits = static_cast<int>(m_visits);
		int int_child_visits = static_cast<int>(child.get_visits());
		int int_parent_visits = static_cast<int>(parentvisits);
		
        auto psa = child.get_policy();
        auto denom = 1.0 + child.get_visits();
        auto puct = cfg_puct * psa * (numerator / denom);
        auto value = winrate + puct;
		auto value_wide_search = winrate + (puct_search_width * puct);
        assert(value > std::numeric_limits<double>::lowest());

		if (is_root
			// && int_m_visits > 800 // This line would allow us to first require LZ to conduct an unmodified optimal search for 800 visits before allowing the widened search to happen, but is commented out. The widened search instead currently starts immediately
			&& int_child_visits > ((visit_search_width * int_m_visits) + 1)) { // Forces LZ to skip visits on root nodes which exceed a certain percentage of total visits conducted so far. The "search_width" multiplier is the maximum allowed percentage. LZ still chooses moves according to its regular "value = winrate + puct" calculation--we simply force it to spend visits on a wider selection of its top move choices.
			continue; // This skips this move. Must be fixed later to handle race conditions which could very rarely crash LZ.

		}

		if (value > best_value) {
			best_value = value;
			best = &child;
		}

		if (is_root &&
			(value_wide_search > best_value)) {
			best = &child;
		}

		if (!is_root && 
			(value > best_value)) {
			best_value = value;
			best = &child;
		}
    }

    assert(best != nullptr);
    best->inflate();
    return best->get();
}

class NodeComp : public std::binary_function<UCTNodePointer&,
                                             UCTNodePointer&, bool> {
public:
    NodeComp(int color) : m_color(color) {};
    bool operator()(const UCTNodePointer& a,
                    const UCTNodePointer& b) {
        // if visits are not same, sort on visits
        if (a.get_visits() != b.get_visits()) {
            return a.get_visits() < b.get_visits();
        }

        // neither has visits, sort on policy prior
        if (a.get_visits() == 0) {
            return a.get_policy() < b.get_policy();
        }

        // both have same non-zero number of visits
        return a.get_eval(m_color) < b.get_eval(m_color);
    }
private:
    int m_color;
};

void UCTNode::sort_children(int color) {
    std::stable_sort(rbegin(m_children), rend(m_children), NodeComp(color));
}

UCTNode& UCTNode::get_best_root_child(int color) {
    wait_expanded();

    assert(!m_children.empty());

    auto ret = std::max_element(begin(m_children), end(m_children),
                                NodeComp(color));
    ret->inflate();

    return *(ret->get());
}

size_t UCTNode::count_nodes_and_clear_expand_state() {
    auto nodecount = size_t{0};
    nodecount += m_children.size();
    if (expandable()) {
        m_expand_state = ExpandState::INITIAL;
    }
    for (auto& child : m_children) {
        if (child.is_inflated()) {
            nodecount += child->count_nodes_and_clear_expand_state();
        }
    }
    return nodecount;
}

void UCTNode::invalidate() {
    m_status = INVALID;
}

void UCTNode::set_active(const bool active) {
    if (valid()) {
        m_status = active ? ACTIVE : PRUNED;
    }
}

bool UCTNode::valid() const {
    return m_status != INVALID;
}

bool UCTNode::active() const {
    return m_status == ACTIVE;
}

bool UCTNode::acquire_expanding() {
    auto expected = ExpandState::INITIAL;
    auto newval = ExpandState::EXPANDING;
    return m_expand_state.compare_exchange_strong(expected, newval);
}

void UCTNode::expand_done() {
    auto v = m_expand_state.exchange(ExpandState::EXPANDED);
#ifdef NDEBUG
    (void)v;
#endif
    assert(v == ExpandState::EXPANDING);
}
void UCTNode::expand_cancel() {
    auto v = m_expand_state.exchange(ExpandState::INITIAL);
#ifdef NDEBUG
    (void)v;
#endif
    assert(v == ExpandState::EXPANDING);
}
void UCTNode::wait_expanded() {
    while (m_expand_state.load() == ExpandState::EXPANDING) {}
    auto v = m_expand_state.load();
#ifdef NDEBUG
    (void)v;
#endif
    assert(v == ExpandState::EXPANDED);
}

