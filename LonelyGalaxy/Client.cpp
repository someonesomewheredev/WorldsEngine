#include "Client.hpp"
#include <Core/Log.hpp>
#include "NetMessage.hpp"

namespace lg {
    Client::Client() : serverPeer {nullptr} {
        host = enet_host_create(nullptr, 1, NetChannel_Count, 0, 0);
    }

    void Client::connect(ENetAddress address) {
        serverPeer = enet_host_connect(host, &address, NetChannel_Count, 0);
    }

    void Client::sendPacketToServer(ENetPacket* p, NetChannel channel) {
        int result = enet_peer_send(serverPeer, channel, p);

        if (result != 0) {
            logWarn("failed to send packet");
        }
    }

    void Client::setClientInfo(uint64_t gameVersion, uint64_t userAuthId, uint16_t userAuthUniverse) {
        this->gameVersion = gameVersion;
        this->userAuthId = userAuthId;
        this->userAuthUniverse = userAuthUniverse;
    }

    void Client::disconnect() {
        enet_peer_disconnect_now(serverPeer, DisconnectReason_PlayerLeaving);
    }

    void Client::handleConnection(const ENetEvent& evt) {
        logMsg("connected! ping is %u", serverPeer->roundTripTime);

        msgs::PlayerJoinRequest pjr;
        pjr.gameVersion = gameVersion;
        pjr.userAuthId = userAuthId;
        pjr.userAuthUniverse = userAuthUniverse;

        auto pjrPacket = pjr.toPacket(ENET_PACKET_FLAG_RELIABLE);

        sendPacketToServer(pjrPacket);
    }

    void Client::handleDisconnection(const ENetEvent& evt) {
        logMsg("disconnected :( reason was %u", evt.data);
    }

    void Client::handleReceivedPacket(const ENetEvent& evt, MessageCallback callback) {
        if (evt.packet->data[0] == MessageType::JoinAccept) {
            msgs::PlayerJoinAcceptance pja;
            pja.fromPacket(evt.packet);

            logMsg("join accepted! :)");
            logMsg("our server side id is %i", pja.serverSideID);
            serverSideID = pja.serverSideID;

            enet_packet_destroy(evt.packet);
            return;
        }

        NetBase::handleReceivedPacket(evt, callback);
    }

    Client::~Client() {
        if (serverPeer)
            enet_peer_disconnect_now(serverPeer, DisconnectReason_PlayerLeaving);
    }
}
