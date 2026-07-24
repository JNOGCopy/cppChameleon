#pragma once
#include <packet.h>

#include <chrono>
#include <string>
#include <vector>


struct ServerGameplay
{

	ENetHost *server = nullptr;

	bool init();

	void update();

	void close();

	void addConnection(ENetEvent& event);
	
	void recieveDataFromClients(ENetEvent& event);
	
	void removeConnection(ENetEvent& event);

	bool startGame(std::uint64_t requestedHunterCID);
	void endGame();
	bool setHunterCID(std::uint64_t requestedHunterCID);
	bool hasConnectedClient(std::uint64_t cid) const;
	bool isHiderFound(std::uint64_t cid) const;
	std::size_t getRemainingHiderCount() const;
	std::uint32_t getCurrentRoundTimerSeconds() const;
	const char *getRoundPhaseName() const;
	void resetRoundProgress();
	ENetPeer *findPeerByCid(std::uint64_t cid);

	void syncGameStateToPeer(ENetPeer *peer);
	void broadcastPacketToAllClients(Packet packet,
		const char *data, size_t size, bool reliable, int channel);

	void broadcastPacketToOtherClients(ENetPeer *sourcePeer, Packet packet,
		const char *data, size_t size, bool reliable, int channel);

	std::uint64_t getIdAndIncrement();

	std::uint64_t playerIDs = 1;
	std::uint32_t connectedClients = 0;
	bool gameActive = false;
	std::uint64_t hunterCID = 0;
	int hiderTimerSeconds = 30;
	int hunterTimerSeconds = 120;
	std::uint32_t roundPhase = roundPhaseLobby;
	std::chrono::steady_clock::time_point roundPhaseDeadline = {};
	bool roundPhaseTimerRunning = false;
	std::vector<std::uint64_t> connectedClientIDs;
	std::vector<std::uint64_t> foundHiderIDs;
	std::string lastStatus = "Server stopped.";
};
