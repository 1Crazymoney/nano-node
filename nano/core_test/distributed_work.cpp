#include <nano/core_test/testutil.hpp>
#include <nano/node/testing.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST (distributed_work, stopped)
{
	nano::system system (24000, 1);
	system.nodes[0]->distributed_work.stop ();
	ASSERT_TRUE (system.nodes[0]->distributed_work.make (nano::block_hash (), {}, {}, nano::network_constants::publish_test_threshold));
}

TEST (distributed_work, no_peers)
{
	nano::system system (24000, 1);
	auto node (system.nodes[0]);
	nano::block_hash hash{ 1 };
	boost::optional<uint64_t> work;
	std::atomic<bool> done{ false };
	auto callback = [&work, &done](boost::optional<uint64_t> work_a) {
		ASSERT_TRUE (work_a.is_initialized ());
		work = work_a;
		done = true;
	};
	ASSERT_FALSE (node->distributed_work.make (hash, node->config.work_peers, callback, node->network_params.network.publish_threshold, nano::account ()));
	system.deadline_set (5s);
	while (!done)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_FALSE (nano::work_validate (hash, *work));
	// should only be removed after cleanup
	ASSERT_EQ (1, node->distributed_work.items.size ());
	while (!node->distributed_work.items.empty ())
	{
		node->distributed_work.cleanup_finished ();
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (distributed_work, no_peers_disabled)
{
	nano::system system (24000, 0);
	nano::node_config node_config (24000, system.logging);
	node_config.work_threads = 0;
	auto & node = *system.add_node (node_config);
	ASSERT_TRUE (node.distributed_work.make (nano::block_hash (), node.config.work_peers, {}, nano::network_constants::publish_test_threshold));
}

TEST (distributed_work, no_peers_cancel)
{
	nano::system system (24000, 0);
	nano::node_config node_config (24000, system.logging);
	node_config.max_work_generate_multiplier = 1e6;
	node_config.max_work_generate_difficulty = nano::difficulty::from_multiplier (node_config.max_work_generate_multiplier, nano::network_constants::publish_test_threshold);
	auto & node = *system.add_node (node_config);
	nano::block_hash hash{ 1 };
	bool done{ false };
	auto callback_to_cancel = [&done](boost::optional<uint64_t> work_a) {
		ASSERT_FALSE (work_a.is_initialized ());
		done = true;
	};
	ASSERT_FALSE (node.distributed_work.make (hash, node.config.work_peers, callback_to_cancel, nano::difficulty::from_multiplier (1e6, node.network_params.network.publish_threshold)));
	ASSERT_EQ (1, node.distributed_work.items.size ());
	// cleanup should not cancel or remove an ongoing work
	node.distributed_work.cleanup_finished ();
	ASSERT_EQ (1, node.distributed_work.items.size ());

	// manually cancel
	node.distributed_work.cancel (hash, true); // forces local stop
	system.deadline_set (20s);
	while (!done && !node.distributed_work.items.empty ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	// now using observer
	done = false;
	ASSERT_FALSE (node.distributed_work.make (hash, node.config.work_peers, callback_to_cancel, nano::difficulty::from_multiplier (1000000, node.network_params.network.publish_threshold)));
	ASSERT_EQ (1, node.distributed_work.items.size ());
	node.observers.work_cancel.notify (hash);
	system.deadline_set (20s);
	while (!done && !node.distributed_work.items.empty ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (distributed_work, no_peers_multi)
{
	nano::system system (24000, 1);
	auto node (system.nodes[0]);
	nano::block_hash hash{ 1 };
	unsigned total{ 10 };
	std::atomic<unsigned> count{ 0 };
	auto callback = [&count](boost::optional<uint64_t> work_a) {
		ASSERT_TRUE (work_a.is_initialized ());
		++count;
	};
	// Test many works for the same root
	for (unsigned i{ 0 }; i < total; ++i)
	{
		ASSERT_FALSE (node->distributed_work.make (hash, node->config.work_peers, callback, nano::difficulty::from_multiplier (10, node->network_params.network.publish_threshold)));
	}
	// 1 root, and _total_ requests for that root are expected, but some may have already finished
	ASSERT_EQ (1, node->distributed_work.items.size ());
	{
		auto requests (node->distributed_work.items.begin ());
		ASSERT_EQ (hash, requests->first);
		ASSERT_GE (requests->second.size (), total - 4);
	}
	system.deadline_set (5s);
	while (count < total)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (node->distributed_work.items.empty ())
	{
		node->distributed_work.cleanup_finished ();
		ASSERT_NO_ERROR (system.poll ());
	}
	count = 0;
	// Test many works for different roots
	for (unsigned i{ 0 }; i < total; ++i)
	{
		nano::block_hash hash_i (i + 1);
		ASSERT_FALSE (node->distributed_work.make (hash_i, node->config.work_peers, callback, node->network_params.network.publish_threshold));
	}
	// 10 roots expected with 1 work each, but some may have completed so test for some
	ASSERT_GT (node->distributed_work.items.size (), 5);
	for (auto & requests : node->distributed_work.items)
	{
		ASSERT_EQ (1, requests.second.size ());
	}
	system.deadline_set (5s);
	while (count < total)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (node->distributed_work.items.empty ())
	{
		node->distributed_work.cleanup_finished ();
		ASSERT_NO_ERROR (system.poll ());
	}
	count = 0;
}
