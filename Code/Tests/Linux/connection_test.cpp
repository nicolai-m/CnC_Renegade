// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "connect.h"
#include "netutil.h"
#include "packetmgr.h"

namespace {

constexpr ULONG LoopbackHost = 0x7f000001u;
constexpr UINT ProtocolToken = 0x52454e45u;
constexpr UINT ClientPayload = 0x12345678u;
constexpr UINT ServerPayload = 0xabcdef01u;
constexpr int Bandwidth = 128000;

int accepted_client_id = ID_UNKNOWN;
bool client_accepted = false;
REFUSAL_CODE client_refusal = REFUSAL_CLIENT_ACCEPTED;
int requests = 0;
int server_messages = 0;
int client_messages = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

REFUSAL_CODE Check_Request(cPacket &packet)
{
    ++requests;
    UINT token = 0;
    return packet.Get(token) == ProtocolToken ? REFUSAL_CLIENT_ACCEPTED : REFUSAL_VERSION_MISMATCH;
}

void Connected(int id)
{
    Check(accepted_client_id == ID_UNKNOWN, "Server accepted the same connection twice");
    accepted_client_id = id;
}

void Accepted()
{
    Check(!client_accepted, "Client received multiple acceptance callbacks");
    client_accepted = true;
}

void Refused(REFUSAL_CODE code)
{
    client_refusal = code;
}

void Server_Message(cPacket &packet, int id)
{
    Check(id == accepted_client_id, "Reliable message had the wrong peer ID");
    UINT value = 0;
    Check(packet.Get(value) == ClientPayload && packet.Is_Flushed(), "Server received an incorrect payload");
    ++server_messages;
}

void Client_Message(cPacket &packet)
{
    UINT value = 0;
    Check(packet.Get(value) == ServerPayload && packet.Is_Flushed(), "Client received an incorrect payload");
    ++client_messages;
}

void Install_Client_Handlers(cConnection &client)
{
    client.Install_Accept_Handler(Accepted);
    client.Install_Refusal_Handler(Refused);
    client.Install_Client_Packet_Handler(Client_Message);
}

template<class Predicate>
bool Service_Until(cConnection &server, cConnection &client, Predicate done)
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        client.Service_Read();
        client.Service_Send(true);
        server.Service_Read();
        server.Service_Send(true);
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void Connect(cConnection &client, const cConnection &server, UINT token)
{
    client.Init_As_Client(htonl(LoopbackHost), server.Get_Local_Port());
    Check(client.Get_Local_Port() != 0, "Client failed to bind an ephemeral port");
    client.Set_Bandwidth_Budget_Out(Bandwidth);
    Install_Client_Handlers(client);
    cPacket request;
    request.Add(token);
    request.Add(Bandwidth);
    client.Connect_Cs(request);
}

} // namespace

int main()
{
    Check(cNetUtil::Protocol_Init(false), "IPv4 unavailable");
    PacketManager.Set_Is_Server(true);

    cConnection server;
    Check(server.Init_As_Server(0, 2, true, LoopbackHost) && server.Get_Local_Port() != 0,
        "Dedicated connection could not bind an ephemeral loopback port");
    server.Set_Bandwidth_Budget_Out(Bandwidth);
    server.Install_Application_Acceptance_Handler(Check_Request);
    server.Install_Conn_Handler(Connected);
    server.Install_Server_Packet_Handler(Server_Message);

    {
        cConnection client;
        Connect(client, server, ProtocolToken);
        Check(Service_Until(server, client, [&] { return client_accepted && client.Is_Established(); }),
            "Legacy Renegade handshake did not establish a session");
        Check(accepted_client_id > 0 && client.Get_Local_Id() == accepted_client_id && requests == 1,
            "Client and server disagreed about the assigned peer ID");

        cPacket client_packet;
        client_packet.Add(ClientPayload);
        client.Send_Packet_To_Individual(client_packet, SERVER_RHOST_ID, SEND_RELIABLE);
        cPacket server_packet;
        server_packet.Add(ServerPayload);
        server.Send_Packet_To_Individual(server_packet, accepted_client_id, SEND_RELIABLE);
        Check(Service_Until(server, client, [] { return server_messages == 1 && client_messages == 1; }),
            "The established connection did not deliver reliable data in both directions");
        // Drain the final ACK and confirm both reliable send queues are cleared.
        Check(Service_Until(server, client, [&] {
            return client.Get_Remote_Host(SERVER_RHOST_ID)->Get_Packet_List(RELIABLE_SEND_LIST).Is_Empty() &&
                server.Get_Remote_Host(accepted_client_id)->Get_Packet_List(RELIABLE_SEND_LIST).Is_Empty();
        }), "Reliable messages were not acknowledged");
    }
    server.Destroy_Connection(accepted_client_id);

    cConnection refused_client;
    Connect(refused_client, server, 0);
    Check(Service_Until(server, refused_client, [&] { return refused_client.Is_Destroy(); }) &&
        client_refusal == REFUSAL_VERSION_MISMATCH && !refused_client.Is_Established() && requests == 2,
        "Server did not return the application refusal code");
}
