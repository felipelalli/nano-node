#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/timer.hpp>
#include <nano/secure/common.hpp>

#include <boost/circular_buffer.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/pool/pool_alloc.hpp>
#include <boost/thread/thread.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <queue>
#include <set>
#include <unordered_map>

namespace nano
{
class node;
class block;
class block_sideband;
class vote;
class election;
class transaction;

class conflict_info final
{
public:
	nano::qualified_root root;
	uint64_t difficulty;
	uint64_t adjusted_difficulty;
	std::shared_ptr<nano::election> election;
};

enum class election_status_type : uint8_t
{
	ongoing = 0,
	active_confirmed_quorum = 1,
	active_confirmation_height = 2,
	inactive_confirmation_height = 3,
	rpc_confirmation_height = 4,
	stopped = 5
};

class election_status final
{
public:
	std::shared_ptr<nano::block> winner;
	nano::amount tally;
	std::chrono::milliseconds election_end;
	std::chrono::milliseconds election_duration;
	election_status_type type;
};

class cementable_account final
{
public:
	cementable_account (nano::account const & account_a, size_t blocks_uncemented_a);
	nano::account account;
	uint64_t blocks_uncemented{ 0 };
};

// Core class for determining consensus
// Holds all active blocks i.e. recently added blocks that need confirmation
class active_transactions final
{
public:
	explicit active_transactions (nano::node &, bool delay_frontier_confirmation_height_updating = false);
	~active_transactions ();
	// Start an election for a block
	// Call action with confirmed block, may be different than what we started with
	// clang-format off
	bool start (std::shared_ptr<nano::block>, std::function<void(std::shared_ptr<nano::block>)> const & = [](std::shared_ptr<nano::block>) {});
	// clang-format on
	// If this returns true, the vote is a replay
	// If this returns false, the vote may or may not be a replay
	bool vote (std::shared_ptr<nano::vote>, bool = false);
	// Is the root of this block in the roots container
	bool active (nano::block const &);
	bool active (nano::qualified_root const &);
	void update_difficulty (nano::block const &);
	void adjust_difficulty (nano::block_hash const &);
	void update_active_difficulty (std::unique_lock<std::mutex> &);
	uint64_t active_difficulty ();
	std::deque<std::shared_ptr<nano::block>> list_blocks (bool = false);
	void erase (nano::block const &);
	//drop 2 from roots based on adjusted_difficulty
	void flush_lowest ();
	bool empty ();
	size_t size ();
	void stop ();
	bool publish (std::shared_ptr<nano::block> block_a);
	void confirm_block (nano::transaction const &, std::shared_ptr<nano::block>, nano::block_sideband const &);
	boost::multi_index_container<
	nano::conflict_info,
	boost::multi_index::indexed_by<
	boost::multi_index::hashed_unique<
	boost::multi_index::member<nano::conflict_info, nano::qualified_root, &nano::conflict_info::root>>,
	boost::multi_index::ordered_non_unique<
	boost::multi_index::member<nano::conflict_info, uint64_t, &nano::conflict_info::adjusted_difficulty>,
	std::greater<uint64_t>>>>
	roots;
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::election>> blocks;
	std::deque<nano::election_status> list_confirmed ();
	std::deque<nano::election_status> confirmed;
	nano::node & node;
	std::mutex mutex;
	// Maximum number of conflicts to vote on per interval, lowest root hash first
	static unsigned constexpr announcements_per_interval = 32;
	// Minimum number of block announcements
	static unsigned constexpr announcement_min = 2;
	// Threshold to start logging blocks haven't yet been confirmed
	static unsigned constexpr announcement_long = 2;
	size_t long_unconfirmed_size = 0;
	static size_t constexpr max_broadcast_queue = 1000;
	boost::circular_buffer<double> multipliers_cb;
	uint64_t trended_active_difficulty;
	size_t priority_cementable_frontiers_size ();

private:
	// Call action with confirmed block, may be different than what we started with
	// clang-format off
	bool add (std::shared_ptr<nano::block>, std::function<void(std::shared_ptr<nano::block>)> const & = [](std::shared_ptr<nano::block>) {});
	// clang-format on
	void request_loop ();
	void request_confirm (std::unique_lock<std::mutex> &);
	void confirm_frontiers (nano::transaction const &);
	nano::account next_frontier_account{ 0 };
	std::chrono::steady_clock::time_point next_frontier_check{ std::chrono::steady_clock::now () };
	std::condition_variable condition;
	bool started{ false };
	std::atomic<bool> stopped{ false };
	void prioritize_frontiers_for_confirmation (nano::transaction const &, std::chrono::milliseconds);
	boost::multi_index_container<
	nano::cementable_account,
	boost::multi_index::indexed_by<
	boost::multi_index::hashed_unique<
	boost::multi_index::member<nano::cementable_account, nano::account, &nano::cementable_account::account>>,
	boost::multi_index::ordered_non_unique<
	boost::multi_index::member<nano::cementable_account, uint64_t, &nano::cementable_account::blocks_uncemented>,
	std::greater<uint64_t>>>>
	priority_cementable_frontiers;
	static size_t constexpr max_priority_cementable_frontiers{ 100000 };
	static size_t constexpr confirmed_frontiers_max_pending_cut_off{ 1000 };
	boost::thread thread;

	friend class confirmation_height_prioritize_frontiers_Test;
};

std::unique_ptr<seq_con_info_component> collect_seq_con_info (active_transactions & active_transactions, const std::string & name);
}
