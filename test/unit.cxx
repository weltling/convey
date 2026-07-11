// Unit tests for the pure logic in main.cxx.
// Defines CONVEY_UNIT_TEST so main.cxx omits its own main(), then includes it
// and exercises its static functions directly.
#define CONVEY_UNIT_TEST
#include "../main.cxx"

static int g_fail = 0;
#define EXPECT(c) do { \
	if (!(c)) { std::cerr << "FAIL: " #c << std::endl; ++g_fail; } \
	else { std::cout << "PASS: " #c << std::endl; } \
} while (0)

// Drive convey_conf_setup with a fresh global conf and argv built from args.
static convey_setup_status run_setup(std::vector<const char*> args)
{
	conf = convey_conf{};
	restart_on_exit = false;
	std::vector<char*> argv;
	for (const char* a : args) {
		argv.push_back(const_cast<char*>(a));
	}
	return convey_conf_setup(static_cast<int>(argv.size()), argv.data());
}

int main()
{
	{
		convey_transport_spec s = convey_parse_transport("tcp:127.0.0.1:4445");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_tcp_client);
		EXPECT(s.host == "127.0.0.1");
		EXPECT(s.port == "4445");
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp:server.local:22");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_tcp_client);
		EXPECT(s.host == "server.local");
		EXPECT(s.port == "22");
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp-listen:5000");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_tcp_server);
		EXPECT(s.port == "5000");
	}
	{
		convey_transport_spec s = convey_parse_transport("\\\\.\\pipe\\kd0");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_pipe);
	}
	{
		convey_transport_spec s = convey_parse_transport("COM3");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_pipe);
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp:");
		EXPECT(!s.ok);
		EXPECT(s.kind == convey_tp_tcp_client);
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp:host");
		EXPECT(!s.ok);
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp:host:");
		EXPECT(!s.ok);
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp-listen:");
		EXPECT(!s.ok);
		EXPECT(s.kind == convey_tp_tcp_server);
	}

	EXPECT(convey_parity_from_string("even") == EVENPARITY);
	EXPECT(convey_parity_from_string("NO") == NOPARITY);
	EXPECT(convey_parity_from_string("Space") == SPACEPARITY);
	EXPECT(convey_parity_from_string("bogus") == (uint8_t)-1);

	EXPECT(convey_stop_bits_from_string("1") == ONESTOPBIT);
	EXPECT(convey_stop_bits_from_string("1.5") == ONE5STOPBITS);
	EXPECT(convey_stop_bits_from_string("2") == TWOSTOPBITS);
	EXPECT(convey_stop_bits_from_string("3") == (uint8_t)-1);

	EXPECT(convey_flow_control_from_string("none") == convey_flow_control_none);
	EXPECT(convey_flow_control_from_string("XON/XOFF") == convey_flow_control_xonxoff);
	EXPECT(convey_flow_control_from_string("rts/cts") == convey_flow_control_rtscts);
	EXPECT(convey_flow_control_from_string("dsr/dtr") == convey_flow_control_dsrdtr);
	EXPECT(convey_flow_control_from_string("bad") == (convey_flow_control)-1);

	EXPECT(convey_baud_is_valid(CBR_9600));
	EXPECT(convey_baud_is_valid(CBR_115200));
	EXPECT(!convey_baud_is_valid(12345));

	EXPECT(convey_trim_crlf("ab\r\n", 4) == 3);
	EXPECT(convey_trim_crlf("ab\n", 3) == 3);
	EXPECT(convey_trim_crlf("\r\n", 2) == 1);
	EXPECT(convey_trim_crlf("", 0) == 0);
	EXPECT(convey_trim_crlf("x", 1) == 1);

	{
		char rec[16];
		DWORD n = convey_log_session_record(rec, "hi", 2, true);
		EXPECT(n == 4);
		EXPECT(rec[0] == '>');
		EXPECT(rec[1] == ' ');
		EXPECT(rec[2] == 'h');
		EXPECT(rec[3] == 'i');
	}
	{
		char rec[16];
		DWORD n = convey_log_session_record(rec, "x", 1, false);
		EXPECT(n == 3);
		EXPECT(rec[0] == '<');
		EXPECT(rec[2] == 'x');
	}

	// --- convey_conf_setup: parser functional-equivalence coverage ---
	{
		// Endpoint given as the first positional argument.
		EXPECT(run_setup({"convey", "tcp:127.0.0.1:4445"}) == convey_setup_ok);
		EXPECT(conf.pipe_path == "tcp:127.0.0.1:4445");
		EXPECT(conf.transport == convey_tp_tcp_client);
		EXPECT(conf.tcp_host == "127.0.0.1");
		EXPECT(conf.tcp_port == "4445");
	}
	{
		// --dev is an alias for the positional endpoint.
		EXPECT(run_setup({"convey", "--dev", "\\\\.\\pipe\\kd0"}) == convey_setup_ok);
		EXPECT(conf.pipe_path == "\\\\.\\pipe\\kd0");
		EXPECT(conf.transport == convey_tp_pipe);
	}
	{
		// tcp-listen endpoint.
		EXPECT(run_setup({"convey", "tcp-listen:5000"}) == convey_setup_ok);
		EXPECT(conf.transport == convey_tp_tcp_server);
		EXPECT(conf.tcp_port == "5000");
	}
	{
		// A missing endpoint is an error.
		EXPECT(run_setup({"convey"}) == convey_setup_exit_err);
	}
	{
		// An unknown option is an error.
		EXPECT(run_setup({"convey", "--nope", "COM1"}) == convey_setup_exit_err);
	}
	{
		// Defaults match the documented values.
		EXPECT(run_setup({"convey", "COM1"}) == convey_setup_ok);
		EXPECT(conf.baud == CBR_115200);
		EXPECT(conf.parity == NOPARITY);
		EXPECT(conf.stop_bits == ONESTOPBIT);
		EXPECT(conf.byte_size == 8);
		EXPECT(conf.flow_control == convey_flow_control_none);
		EXPECT(!restart_on_exit);
	}
	{
		// Serial parameters are parsed and mapped.
		EXPECT(run_setup({"convey", "--baud", "9600", "--parity", "even",
			"--stop-bits", "2", "--byte-size", "7", "--flow-control", "rts/cts", "COM1"}) == convey_setup_ok);
		EXPECT(conf.baud == CBR_9600);
		EXPECT(conf.parity == EVENPARITY);
		EXPECT(conf.stop_bits == TWOSTOPBITS);
		EXPECT(conf.byte_size == 7);
		EXPECT(conf.flow_control == convey_flow_control_rtscts);
	}
	{
		// Invalid serial values are rejected.
		EXPECT(run_setup({"convey", "--baud", "12345", "COM1"}) == convey_setup_exit_err);
		EXPECT(run_setup({"convey", "--parity", "bogus", "COM1"}) == convey_setup_exit_err);
		EXPECT(run_setup({"convey", "--stop-bits", "3", "COM1"}) == convey_setup_exit_err);
		EXPECT(run_setup({"convey", "--flow-control", "bad", "COM1"}) == convey_setup_exit_err);
	}
	{
		// --reconnect arms the restart loop.
		EXPECT(run_setup({"convey", "--reconnect", "tcp:127.0.0.1:9"}) == convey_setup_ok);
		EXPECT(restart_on_exit);
	}
	{
		// --bridge requires --pipe-server, then arms the restart loop.
		EXPECT(run_setup({"convey", "--bridge", "tcp:127.0.0.1:9"}) == convey_setup_exit_err);
		EXPECT(run_setup({"convey", "--bridge", "--pipe-server", "\\\\.\\pipe\\b", "tcp:127.0.0.1:9"}) == convey_setup_ok);
		EXPECT(conf.bridge);
		EXPECT(conf.bridge_pipe_name == "\\\\.\\pipe\\b");
		EXPECT(restart_on_exit);
	}
	{
		// Log options land in the right fields.
		EXPECT(run_setup({"convey", "--log", "a.log", "--log-recv", "b.log",
			"--log-send", "c.log", "--log-append", "COM1"}) == convey_setup_ok);
		EXPECT(conf.log_path == "a.log");
		EXPECT(conf.log_recv_path == "b.log");
		EXPECT(conf.log_send_path == "c.log");
		EXPECT(conf.log_append);
	}
	{
		// Two log options may not share a file.
		EXPECT(run_setup({"convey", "--log", "x.log", "--log-recv", "x.log", "COM1"}) == convey_setup_exit_err);
	}
	{
		// --no-xterm and --verbose flags.
		EXPECT(run_setup({"convey", "--no-xterm", "--verbose", "COM1"}) == convey_setup_ok);
		EXPECT(conf.no_xterm);
		EXPECT(conf.verbose);
	}

	if (g_fail) {
		std::cerr << g_fail << " unit test(s) failed." << std::endl;
		return 1;
	}
	std::cout << "All unit tests passed." << std::endl;
	return 0;
}
