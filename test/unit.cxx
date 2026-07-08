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

	if (g_fail) {
		std::cerr << g_fail << " unit test(s) failed." << std::endl;
		return 1;
	}
	std::cout << "All unit tests passed." << std::endl;
	return 0;
}
