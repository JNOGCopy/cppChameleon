#include "gameLayer.h"
#include "ClientGameplay.h"
#include "GameMaps.h"
#include "ServerGameplay.h"

#include <enet/enet.h>
#include <gl2d/gl2d.h>
#include <glad/glad.h>
#include <glui/glui.h>

#include <algorithm>
#include <climits>

gl2d::Renderer2D renderer;
ClientGameplay clientGameplay;
ServerGameplay serverGameplay;

glui::RendererUi rendererUI;
gl2d::Texture uiTexture;
gl2d::Font font;
char serverIpAddress[128] = "localhost";
int hunterIdSliderValue = 1;
int mapIndexSliderValue = 0;
std::string menuStatusMessage;
bool enetInitialized = false;

namespace
{
	int getHunterIdSliderMax()
	{
		int maxId = 1;
		for (std::uint64_t cid : serverGameplay.connectedClientIDs)
		{
			maxId = (std::max)(maxId, static_cast<int>((std::min)(cid, static_cast<std::uint64_t>(INT_MAX))));
		}

		return maxId;
	}

	std::uint64_t resolveHunterIdSliderSelection()
	{
		if (serverGameplay.connectedClientIDs.empty())
		{
			return 0;
		}

		const int sliderValue = std::clamp(hunterIdSliderValue, 1, getHunterIdSliderMax());
		std::uint64_t bestCid = serverGameplay.connectedClientIDs.front();
		int bestDistance = std::abs(static_cast<int>(bestCid) - sliderValue);

		for (std::uint64_t cid : serverGameplay.connectedClientIDs)
		{
			const int clampedCid = static_cast<int>((std::min)(cid, static_cast<std::uint64_t>(INT_MAX)));
			const int distance = std::abs(clampedCid - sliderValue);
			if (distance < bestDistance)
			{
				bestCid = cid;
				bestDistance = distance;
			}
		}

		return bestCid;
	}

	void syncHunterIdSliderToServerSelection()
	{
		if (serverGameplay.hunterCID != 0)
		{
			hunterIdSliderValue = static_cast<int>((std::min)(serverGameplay.hunterCID, static_cast<std::uint64_t>(INT_MAX)));
		}
		else if (!serverGameplay.connectedClientIDs.empty())
		{
			hunterIdSliderValue = static_cast<int>((std::min)(serverGameplay.connectedClientIDs.front(), static_cast<std::uint64_t>(INT_MAX)));
		}
		else
		{
			hunterIdSliderValue = 1;
		}
	}

	void syncMapIndexSliderToServerSelection()
	{
		mapIndexSliderValue = static_cast<int>(clampGameMapIndex(serverGameplay.currentMapIndex));
	}

	std::string buildConnectedIdsText()
	{
		if (serverGameplay.connectedClientIDs.empty())
		{
			return "none";
		}

		std::string result;
		for (size_t i = 0; i < serverGameplay.connectedClientIDs.size(); ++i)
		{
			if (!result.empty())
			{
				result += ", ";
			}

			result += std::to_string(serverGameplay.connectedClientIDs[i]);
		}

		return result;
	}
}


bool initGame()
{
	if (enet_initialize() != 0)
	{
		return false;
	}
	enetInitialized = true;

	gl2d::init();
	renderer.create();

	uiTexture.loadFromFile(RESOURCES_PATH "ui.png", true, true);
	font.createFromFile(RESOURCES_PATH "Arial.ttf");

	return true;
}


enum gameState
{
	inMainMenu = 0,
	inGame = 1,
	inServer = 2

};

int currentGameState = 0;

bool gameLogic(float deltaTime, platform::Input &input)
{
	const int w = platform::getFrameBufferSizeX();
	const int h = platform::getFrameBufferSizeY();

	glViewport(0, 0, w, h);
	glDisable(GL_DITHER);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	bool result = 1;

	renderer.updateWindowMetrics(w, h);

	if (currentGameState == 0)
	{

		rendererUI.Begin(0);


		rendererUI.Text("C++ Chameleon", Colors_White);
		rendererUI.SetAlignModeFixedSizeWidgets({0, 200});
		rendererUI.InputText("connect IP: ", serverIpAddress, sizeof(serverIpAddress), Colors_White);

		if (rendererUI.Button("Start client", Colors_White, uiTexture))
		{
			clientGameplay.shutdown();

			if (clientGameplay.init(serverIpAddress))
			{
				currentGameState = inGame;
				menuStatusMessage.clear();
			}
			else
			{
				menuStatusMessage = clientGameplay.clientNetworking.lastStatus;
			}
		}

		if (rendererUI.Button("Start server", Colors_White, uiTexture))
		{
			serverGameplay.close();

			if (serverGameplay.init())
			{
				syncHunterIdSliderToServerSelection();
				syncMapIndexSliderToServerSelection();
				currentGameState = inServer;
				menuStatusMessage.clear();
			}
			else
			{
				menuStatusMessage = "Failed to start server on port 7769.";
			}
		}

		if (!menuStatusMessage.empty())
		{
			rendererUI.Text(menuStatusMessage, Colors_Red);
		}

		rendererUI.End();

		rendererUI.renderFrame(renderer, font, platform::getRelMousePosition(),
			platform::isLMousePressed(), platform::isLMouseHeld(), platform::isLMouseReleased(),
			platform::isButtonReleased(platform::Button::Escape), platform::getTypedInput(),
			deltaTime);

	}
	else if (currentGameState == inGame)
	{
		result = clientGameplay.update(deltaTime, input, renderer, font);
	}
	else if (currentGameState == inServer)
	{
		serverGameplay.update();
		const int hunterIdSliderMax = getHunterIdSliderMax();
		const int mapIndexSliderMax = (std::max)(0, static_cast<int>(getGameMapCount()) - 1);
		hunterIdSliderValue = std::clamp(hunterIdSliderValue, 1, hunterIdSliderMax);
		mapIndexSliderValue = std::clamp(mapIndexSliderValue, 0, mapIndexSliderMax);
		const GameMapDefinition &selectedMapDefinition = getGameMapDefinition(static_cast<std::uint32_t>(mapIndexSliderValue));
		const GameMapDefinition &currentMapDefinition = getGameMapDefinition(serverGameplay.currentMapIndex);

		rendererUI.Begin(1);
		rendererUI.Text("Server running on port 7769", Colors_White);
		rendererUI.Text("Clients connected: " + std::to_string(serverGameplay.connectedClients), Colors_White);
		rendererUI.Text("Game state: " + std::string(serverGameplay.gameActive ? "Running" : "Lobby"), Colors_White);
		rendererUI.Text("Round phase: " + std::string(serverGameplay.getRoundPhaseName()), Colors_White);
		rendererUI.Text("Round timer: " + std::to_string(serverGameplay.getCurrentRoundTimerSeconds()) + "s", Colors_White);
		rendererUI.Text("Current map: " + std::string(currentMapDefinition.displayName), Colors_White);
		rendererUI.Text("Connected IDs: " + buildConnectedIdsText(), Colors_White);
		rendererUI.sliderInt("hunter ID: ", &hunterIdSliderValue, 1, hunterIdSliderMax, Colors_White);
		rendererUI.sliderInt("map index: ", &mapIndexSliderValue, 0, mapIndexSliderMax, Colors_White);
		rendererUI.Text("Selected map: " + std::string(selectedMapDefinition.displayName), Colors_White);
		rendererUI.sliderInt("hider timer (s): ", &serverGameplay.hiderTimerSeconds, 0, 500, Colors_White);
		rendererUI.sliderInt("hunter timer (s): ", &serverGameplay.hunterTimerSeconds, 0, 500, Colors_White);

		if (rendererUI.Button("Apply hunter ID", Colors_White, uiTexture))
		{
			serverGameplay.setHunterCID(resolveHunterIdSliderSelection());
			syncHunterIdSliderToServerSelection();
		}

		if (rendererUI.Button("Apply map", Colors_White, uiTexture))
		{
			serverGameplay.setCurrentMapIndex(static_cast<std::uint32_t>(mapIndexSliderValue));
			syncMapIndexSliderToServerSelection();
		}

		if (!serverGameplay.gameActive)
		{
			if (rendererUI.Button("Start game", Colors_White, uiTexture))
			{
				serverGameplay.startGame(resolveHunterIdSliderSelection());
				syncHunterIdSliderToServerSelection();
			}
		}
		else
		{
			if (serverGameplay.roundPhase == roundPhaseHiderHide
				&& rendererUI.Button("Skip hide time", Colors_White, uiTexture))
			{
				serverGameplay.skipHiderHidePhase();
			}

			if (rendererUI.Button("End game", Colors_White, uiTexture))
			{
				serverGameplay.endGame();
				syncHunterIdSliderToServerSelection();
			}
		}

		rendererUI.Text(serverGameplay.lastStatus, Colors_White);
		rendererUI.End();

		rendererUI.renderFrame(renderer, font, platform::getRelMousePosition(),
			platform::isLMousePressed(), platform::isLMouseHeld(), platform::isLMouseReleased(),
			platform::isButtonReleased(platform::Button::Escape), platform::getTypedInput(),
			deltaTime);
	}



	renderer.flush();
	return result;
}

void closeGame()
{
	clientGameplay.shutdown();
	serverGameplay.close();

	if (enetInitialized)
	{
		enet_deinitialize();
		enetInitialized = false;
	}
}
