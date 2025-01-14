#include <nano/core_test/testutil.hpp>
#include <nano/node/daemonconfig.hpp>
#include <nano/node/testing.hpp>
#include <nano/secure/utility.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/program_options.hpp>

#include <iomanip>
#include <random>

/* Boost v1.70 introduced breaking changes; the conditional compilation allows 1.6x to be supported as well. */
#if BOOST_VERSION < 107000
using socket_type = boost::asio::ip::tcp::socket;
#else
using socket_type = boost::asio::basic_stream_socket<boost::asio::ip::tcp, boost::asio::io_context::executor_type>;
#endif

namespace nano
{
void force_nano_test_network ();
}

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

#ifndef BOOST_PROCESS_SUPPORTED
#error BOOST_PROCESS_SUPPORTED must be set, check configuration
#endif

#if BOOST_PROCESS_SUPPORTED
#include <boost/process.hpp>
#endif

constexpr auto rpc_port_start = 60000;
constexpr auto peering_port_start = 61000;
constexpr auto ipc_port_start = 62000;

void write_config_files (boost::filesystem::path const & data_path, int index)
{
	nano::daemon_config daemon_config (data_path);
	nano::jsonconfig json;
	json.read_and_update (daemon_config, data_path / "config.json");
	auto node_l = json.get_required_child ("node");
	node_l.put ("peering_port", peering_port_start + index);
	// Alternate use of memory pool
	node_l.put ("use_memory_pools", (index % 2) == 0);
	auto tcp = node_l.get_required_child ("ipc").get_required_child ("tcp");
	tcp.put ("enable", true);
	tcp.put ("port", ipc_port_start + index);
	json.write (data_path / "config.json");

	nano::rpc_config rpc_config;
	nano::jsonconfig json1;
	json1.read_and_update (rpc_config, data_path / "rpc_config.json");
	json1.put ("port", rpc_port_start + index);
	json1.put ("enable_control", true);
	json1.get_required_child ("process").put ("ipc_port", ipc_port_start + index);
	json1.write (data_path / "rpc_config.json");
}

// Report a failure
void fail (boost::system::error_code ec, char const * what)
{
	std::cerr << what << ": " << ec.message () << "\n";
}

class account final
{
public:
	std::string private_key;
	std::string public_key;
	std::string as_string;
};

class account_info final
{
public:
	bool operator== (account_info const & other)
	{
		return frontier == other.frontier && block_count == other.block_count && balance == other.balance && error == other.error;
	}

	std::string frontier;
	std::string block_count;
	std::string balance;
	bool error{ false };
};

class receive_session final : public std::enable_shared_from_this<receive_session>
{
public:
	receive_session (boost::asio::io_context & ioc, std::atomic<int> & send_calls_remaining, std::string const & wallet, std::string const & account, std::string const & block, tcp::resolver::results_type const & results) :
	socket (ioc),
	strand (socket.get_executor ()),
	send_calls_remaining (send_calls_remaining),
	wallet (wallet),
	account (account),
	block (block),
	results (results)
	{
	}

	void run ()
	{
		auto this_l (shared_from_this ());

		boost::asio::async_connect (this_l->socket, this_l->results.cbegin (), this_l->results.cend (), boost::asio::bind_executor (strand, [this_l](boost::system::error_code const & ec, boost::asio::ip::tcp::resolver::iterator) {
			if (ec)
			{
				return fail (ec, "connect");
			}

			boost::property_tree::ptree request;
			request.put ("action", "receive");
			request.put ("wallet", this_l->wallet);
			request.put ("account", this_l->account);
			request.put ("block", this_l->block);
			std::stringstream ostream;
			boost::property_tree::write_json (ostream, request);

			this_l->req.method (http::verb::post);
			this_l->req.version (11);
			this_l->req.target ("/");
			this_l->req.body () = ostream.str ();
			this_l->req.prepare_payload ();

			http::async_write (this_l->socket, this_l->req, boost::asio::bind_executor (this_l->strand, [this_l](boost::system::error_code ec, std::size_t) {
				if (ec)
				{
					return fail (ec, "write");
				}

				http::async_read (this_l->socket, this_l->buffer, this_l->res, boost::asio::bind_executor (this_l->strand, [this_l](boost::system::error_code ec, std::size_t) {
					if (ec)
					{
						return fail (ec, "read");
					}

					--this_l->send_calls_remaining;

					// Gracefully close the socket
					this_l->socket.shutdown (tcp::socket::shutdown_both, ec);
					if (ec && ec != boost::system::errc::not_connected)
					{
						return fail (ec, "shutdown");
					}
				}));
			}));
		}));
	}

private:
	socket_type socket;
	boost::asio::strand<boost::asio::io_context::executor_type> strand;
	boost::beast::flat_buffer buffer;
	http::request<http::string_body> req;
	http::response<http::string_body> res;
	std::atomic<int> & send_calls_remaining;
	std::string wallet;
	std::string account;
	std::string block;
	tcp::resolver::results_type const & results;
};

class send_session final : public std::enable_shared_from_this<send_session>
{
public:
	send_session (boost::asio::io_context & ioc, std::atomic<int> & send_calls_remaining, std::string const & wallet, std::string const & source, std::string const & destination, tcp::resolver::results_type const & results) :
	io_ctx (ioc),
	socket (ioc),
	strand (socket.get_executor ()),
	send_calls_remaining (send_calls_remaining),
	wallet (wallet),
	source (source),
	destination (destination),
	results (results)
	{
	}

	void run ()
	{
		auto this_l (shared_from_this ());

		boost::asio::async_connect (this_l->socket, this_l->results.cbegin (), this_l->results.cend (), boost::asio::bind_executor (strand, [this_l](boost::system::error_code const & ec, boost::asio::ip::tcp::resolver::iterator) {
			if (ec)
			{
				return fail (ec, "connect");
			}

			boost::property_tree::ptree request;
			request.put ("action", "send");
			request.put ("wallet", this_l->wallet);
			request.put ("source", this_l->source);
			request.put ("destination", this_l->destination);
			request.put ("amount", "1");
			std::stringstream ostream;
			boost::property_tree::write_json (ostream, request);

			this_l->req.method (http::verb::post);
			this_l->req.version (11);
			this_l->req.target ("/");
			this_l->req.body () = ostream.str ();
			this_l->req.prepare_payload ();

			http::async_write (this_l->socket, this_l->req, boost::asio::bind_executor (this_l->strand, [this_l](boost::system::error_code ec, std::size_t) {
				if (ec)
				{
					return fail (ec, "write");
				}

				http::async_read (this_l->socket, this_l->buffer, this_l->res, boost::asio::bind_executor (this_l->strand, [this_l](boost::system::error_code ec, std::size_t) {
					if (ec)
					{
						return fail (ec, "read");
					}

					boost::property_tree::ptree json;
					std::stringstream body (this_l->res.body ());
					boost::property_tree::read_json (body, json);
					auto block = json.get<std::string> ("block");

					std::make_shared<receive_session> (this_l->io_ctx, this_l->send_calls_remaining, this_l->wallet, this_l->destination, block, this_l->results)->run ();

					this_l->socket.shutdown (tcp::socket::shutdown_both, ec);
					if (ec && ec != boost::system::errc::not_connected)
					{
						return fail (ec, "shutdown");
					}
				}));
			}));
		}));
	}

private:
	boost::asio::io_context & io_ctx;
	socket_type socket;
	boost::asio::strand<boost::asio::io_context::executor_type> strand;
	boost::beast::flat_buffer buffer;
	http::request<http::string_body> req;
	http::response<http::string_body> res;
	std::atomic<int> & send_calls_remaining;
	std::string wallet;
	std::string source;
	std::string destination;
	tcp::resolver::results_type const & results;
};

boost::property_tree::ptree rpc_request (boost::property_tree::ptree const & request, boost::asio::io_context & ioc, tcp::resolver::results_type const & results)
{
	tcp::socket socket{ ioc };
	boost::asio::connect (socket, results.begin (), results.end ());

	std::stringstream ostream;
	boost::property_tree::write_json (ostream, request);
	auto request_string = ostream.str ();

	http::request<http::string_body> req{ http::verb::post, "/", 11, request_string };
	req.prepare_payload ();

	http::write (socket, req);
	boost::beast::flat_buffer buffer;

	http::response<boost::beast::http::string_body> res;
	http::read (socket, buffer, res);

	boost::property_tree::ptree json;
	std::stringstream body (res.body ());
	boost::property_tree::read_json (body, json);

	// Gracefully close the socket
	boost::system::error_code ec;
	socket.shutdown (tcp::socket::shutdown_both, ec);

	if (ec && ec != boost::system::errc::not_connected)
	{
		throw boost::system::system_error{ ec };
	}
	return json;
}

void keepalive_rpc (boost::asio::io_context & ioc, tcp::resolver::results_type const & results, uint16_t port)
{
	boost::property_tree::ptree request;
	request.put ("action", "keepalive");
	request.put ("address", "::1");
	request.put ("port", port);

	rpc_request (request, ioc, results);
}

account key_create_rpc (boost::asio::io_context & ioc, tcp::resolver::results_type const & results)
{
	std::string request_string;
	boost::property_tree::ptree request;
	request.put ("action", "key_create");

	auto json = rpc_request (request, ioc, results);

	account account_l;
	account_l.private_key = json.get<std::string> ("private");
	account_l.public_key = json.get<std::string> ("public");
	account_l.as_string = json.get<std::string> ("account");

	return account_l;
}

std::string wallet_create_rpc (boost::asio::io_context & ioc, tcp::resolver::results_type const & results)
{
	std::string request_string;
	boost::property_tree::ptree request;
	request.put ("action", "wallet_create");

	auto json = rpc_request (request, ioc, results);
	return json.get<std::string> ("wallet");
}

void wallet_add_rpc (boost::asio::io_context & ioc, tcp::resolver::results_type const & results, std::string const & wallet, std::string const & prv_key)
{
	boost::property_tree::ptree request;
	request.put ("action", "wallet_add");
	request.put ("wallet", wallet);
	request.put ("key", prv_key);
	rpc_request (request, ioc, results);
}

void stop_rpc (boost::asio::io_context & ioc, tcp::resolver::results_type const & results)
{
	boost::property_tree::ptree request;
	request.put ("action", "stop");
	rpc_request (request, ioc, results);
}

account_info account_info_rpc (boost::asio::io_context & ioc, tcp::resolver::results_type const & results, std::string const & account)
{
	boost::property_tree::ptree request;
	request.put ("action", "account_info");
	request.put ("account", account);

	account_info account_info;
	auto json = rpc_request (request, ioc, results);

	auto error = json.get_optional<std::string> ("error");
	if (error)
	{
		account_info.error = true;
	}
	else
	{
		account_info.balance = json.get<std::string> ("balance");
		account_info.block_count = json.get<std::string> ("block_count");
		account_info.frontier = json.get<std::string> ("frontier");
	}
	return account_info;
}

/** This launches a node and fires a lot of send/recieve RPC requests at it (configurable), then other nodes are tested to make sure they observe these blocks as well. */
int main (int argc, char * const * argv)
{
	nano::force_nano_test_network ();

	boost::program_options::options_description description ("Command line options");

	// clang-format off
	description.add_options ()
		("help", "Print out options")
		("node_count,n", boost::program_options::value<int> ()->default_value (10), "The number of nodes to spin up")
		("send_count,s", boost::program_options::value<int> ()->default_value (2000), "How many send blocks to generate")
		("simultaneous_process_calls", boost::program_options::value<int> ()->default_value (20), "Number of simultaneous rpc sends to do")
		("destination_count", boost::program_options::value<int> ()->default_value (2), "How many destination accounts to choose between")
		("node_path", boost::program_options::value<std::string> (), "The path to the nano_node to test")
		("rpc_path", boost::program_options::value<std::string> (), "The path to do nano_rpc to test");
	// clang-format on

	boost::program_options::variables_map vm;
	try
	{
		boost::program_options::store (boost::program_options::parse_command_line (argc, argv, description), vm);
	}
	catch (boost::program_options::error const & err)
	{
		std::cerr << err.what () << std::endl;
		return 1;
	}
	boost::program_options::notify (vm);

	auto node_count = vm.find ("node_count")->second.as<int> ();
	auto destination_count = vm.find ("destination_count")->second.as<int> ();
	auto send_count = vm.find ("send_count")->second.as<int> ();
	auto simultaneous_process_calls = vm.find ("simultaneous_process_calls")->second.as<int> ();

	boost::system::error_code err;
	auto running_executable_filepath = boost::dll::program_location (err);

	auto node_path_it (vm.find ("node_path"));
	std::string node_path;
	if (node_path_it != vm.end ())
	{
		node_path = node_path_it->second.as<std::string> ();
	}
	else
	{
		auto node_filepath = running_executable_filepath.parent_path () / "nano_node";
		if (running_executable_filepath.has_extension ())
		{
			node_filepath.replace_extension (running_executable_filepath.extension ());
		}
		node_path = node_filepath.string ();
	}

	auto rpc_path_it (vm.find ("rpc_path"));
	std::string rpc_path;
	if (rpc_path_it != vm.end ())
	{
		rpc_path = rpc_path_it->second.as<std::string> ();
	}
	else
	{
		auto rpc_filepath = running_executable_filepath.parent_path () / "nano_rpc";
		if (running_executable_filepath.has_extension ())
		{
			rpc_filepath.replace_extension (running_executable_filepath.extension ());
		}
		rpc_path = rpc_filepath.string ();
	}

	std::vector<boost::filesystem::path> data_paths;
	for (auto i = 0; i < node_count; ++i)
	{
		auto data_path = nano::unique_path ();
		boost::filesystem::create_directory (data_path);
		write_config_files (data_path, i);
		data_paths.push_back (std::move (data_path));
	}

	nano::network_constants network_constants;
	auto current_network = network_constants.get_current_network_as_string ();
#if BOOST_PROCESS_SUPPORTED
	std::vector<std::unique_ptr<boost::process::child>> nodes;
	std::vector<std::unique_ptr<boost::process::child>> rpc_servers;
	for (auto const & data_path : data_paths)
	{
		nodes.emplace_back (std::make_unique<boost::process::child> (node_path, "--daemon", "--data_path", data_path.string (), "--network", current_network));
		rpc_servers.emplace_back (std::make_unique<boost::process::child> (rpc_path, "--daemon", "--data_path", data_path.string (), "--network", current_network));
	}
#else
	std::thread processes_thread ([&data_paths, &node_path, &rpc_path, &current_network]() {
		auto formatted_command = "%1% --daemon --data_path=%2% --network=%3% %4%";
		assert (!data_paths.empty ());
		for (int i = 0; i < data_paths.size (); ++i)
		{
			auto node_exe_command = boost::str (boost::format (formatted_command) % node_path % data_paths[i].string () % current_network % "&");
			auto rpc_exe_command = boost::str (boost::format (formatted_command) % rpc_path % data_paths[i].string () % current_network % "");

			std::system (node_exe_command.c_str ());

			// Makes sure the last command one is not executed in the background
			if (i != data_paths.size () - 1)
			{
				rpc_exe_command += "&";
			}
			std::system (rpc_exe_command.c_str ());
		}
	});
#endif

	std::cout << "Waiting for nodes to spin up..." << std::endl;
	std::this_thread::sleep_for (std::chrono::seconds (7));
	std::cout << "Connecting nodes..." << std::endl;

	boost::asio::io_context ioc;
	tcp::resolver resolver{ ioc };
	auto const primary_node_results = resolver.resolve ("::1", std::to_string (rpc_port_start));

	for (int i = 0; i < node_count; ++i)
	{
		keepalive_rpc (ioc, primary_node_results, peering_port_start + i);
	}

	std::cout << "Beginning tests" << std::endl;

	// Create keys
	std::vector<account> destination_accounts;
	for (int i = 0; i < destination_count; ++i)
	{
		destination_accounts.emplace_back (key_create_rpc (ioc, primary_node_results));
	}

	// Create wallet
	std::string wallet = wallet_create_rpc (ioc, primary_node_results);

	// Add genesis account to it
	wallet_add_rpc (ioc, primary_node_results, wallet, nano::test_genesis_key.prv.data.to_string ());

	// Add destination accounts
	for (auto & account : destination_accounts)
	{
		wallet_add_rpc (ioc, primary_node_results, wallet, account.private_key);
	}

	std::cout << "\rPrimary node processing transactions: 00%";

	std::thread t ([send_count, &destination_accounts, &ioc, &primary_node_results, &wallet, &resolver, &node_count]() {
		std::random_device rd;
		std::mt19937 mt (rd ());
		std::uniform_int_distribution<size_t> dist (0, destination_accounts.size () - 1);

		std::atomic<int> send_calls_remaining{ send_count };

		for (auto i = 0; i < send_count; ++i)
		{
			account * destination_account;
			if (i < destination_accounts.size ())
			{
				destination_account = &destination_accounts[i];
			}
			else
			{
				auto random_account_index = dist (mt);
				destination_account = &destination_accounts[random_account_index];
			}

			std::make_shared<send_session> (ioc, send_calls_remaining, wallet, nano::genesis_account.to_account (), destination_account->as_string, primary_node_results)->run ();
		}

		while (send_calls_remaining != 0)
		{
			static int last_percent = 0;
			auto percent = static_cast<int> (100 * ((send_count - send_calls_remaining) / static_cast<double> (send_count)));

			if (last_percent != percent)
			{
				std::cout << "\rPrimary node processing transactions: " << std::setfill ('0') << std::setw (2) << percent << "%";
				last_percent = percent;
			}
		}

		std::cout << "\rPrimary node processed transactions                " << std::endl;

		std::cout << "Waiting for nodes to catch up..." << std::endl;

		std::map<std::string, account_info> known_account_info;
		for (int i = 0; i < destination_accounts.size (); ++i)
		{
			known_account_info.emplace (destination_accounts[i].as_string, account_info_rpc (ioc, primary_node_results, destination_accounts[i].as_string));
		}

		nano::timer<std::chrono::milliseconds> timer;
		timer.start ();

		for (int i = 1; i < node_count; ++i)
		{
			auto const results = resolver.resolve ("::1", std::to_string (rpc_port_start + i));
			for (auto & account_info : known_account_info)
			{
				while (true)
				{
					auto other_account_info = account_info_rpc (ioc, results, account_info.first);
					if (!other_account_info.error && account_info.second == other_account_info)
					{
						// Found the account in this node
						break;
					}

					if (timer.since_start () > std::chrono::seconds (120))
					{
						throw std::runtime_error ("Timed out");
					}

					std::this_thread::sleep_for (std::chrono::seconds (1));
				}
			}

			stop_rpc (ioc, results);
		}

		// Stop main node
		stop_rpc (ioc, primary_node_results);
	});
	nano::thread_runner runner (ioc, simultaneous_process_calls);
	t.join ();
	runner.join ();

#if BOOST_PROCESS_SUPPORTED
	for (auto & node : nodes)
	{
		node->wait ();
	}
	for (auto & rpc_server : rpc_servers)
	{
		rpc_server->wait ();
	}
#else
	processes_thread.join ();
#endif

	std::cout << "Done!" << std::endl;
}
