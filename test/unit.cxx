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

	if (g_fail) {
		std::cerr << g_fail << " unit test(s) failed." << std::endl;
		return 1;
	}
	std::cout << "All unit tests passed." << std::endl;
	return 0;
}
