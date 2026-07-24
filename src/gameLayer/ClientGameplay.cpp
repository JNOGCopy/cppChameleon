#define GLM_ENABLE_EXPERIMENTAL
#include "ClientGameplay.h"
#include "gameLayer.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/transform.hpp>

#include "platformInput.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iostream>
#include <map>
#include <sstream>

#include "imfilebrowser.h"
#include <gl2d/gl2d.h>
#include <platformTools.h>
#include <IconsForkAwesome.h>
#include <imguiTools.h>
#include <gl3d.h>
#include <imgui_internal.h>

#define PLAYER_ANIMATIONS 18

namespace
{
	constexpr float PLAYER_MODEL_YAW_OFFSET = 3.141592f;
	constexpr float THIRD_PERSON_CAMERA_DISTANCE_DEFAULT = 7.5f;
	constexpr float THIRD_PERSON_CAMERA_DISTANCE_MIN = 2.5f;
	constexpr float THIRD_PERSON_CAMERA_DISTANCE_MAX = 12.0f;
	constexpr float THIRD_PERSON_CAMERA_ZOOM_STEP = 0.85f;
	constexpr float THIRD_PERSON_CAMERA_TARGET_HEIGHT = 1.45f;
	constexpr float HUNTER_VISUAL_SCALE = 1.75f;
	const float THIRD_PERSON_PITCH_MIN = glm::radians(-70.0f);
	const float THIRD_PERSON_PITCH_MAX = glm::radians(35.0f);
	constexpr int PLAYER_PAINT_BRUSH_RADIUS_DEFAULT = 6;
	constexpr int PLAYER_PAINT_BRUSH_RADIUS_MIN = 1;
	constexpr int PLAYER_PAINT_BRUSH_RADIUS_MAX = 96;
	constexpr float PLAYER_PAINT_BRUSH_RESIZE_SPEED = 0.12f;
	constexpr float PLAYER_PAINT_BRUSH_PREVIEW_SCALE = 2.4f;
	constexpr float PAINT_COLOR_PANEL_MARGIN = 18.0f;
	constexpr float PAINT_COLOR_PANEL_WIDTH = 276.0f;
	constexpr float PAINT_COLOR_PANEL_HEIGHT = 172.0f;
	constexpr float PAINT_COLOR_SLIDER_WIDTH = 170.0f;
	constexpr float PAINT_COLOR_SLIDER_HEIGHT = 18.0f;
	constexpr float PAINT_COLOR_SLIDER_SEGMENTS = 40.0f;
	constexpr float PLAYER_STATE_SEND_INTERVAL_UNRELIABLE = 1.0f / 20.0f;
	constexpr float PLAYER_STATE_SEND_INTERVAL_RELIABLE = 1.0f;
	constexpr float PLAYER_PAINT_TEXTURE_SEND_INTERVAL = 0.5f;
	constexpr float PLAYER_STATE_POSITION_EPSILON = 0.02f;
	constexpr float PLAYER_STATE_YAW_EPSILON = 0.02f;
	constexpr float REMOTE_PLAYER_INTERPOLATION_SPEED = 22.0f;
	constexpr float REMOTE_PLAYER_SNAP_DISTANCE = 4.0f;
	constexpr float HUNTER_SHOT_MAX_DISTANCE = 120.0f;
	constexpr float PLAYER_RAYCAST_SPHERE_RADIUS = 0.38f;
	constexpr char HUNTER_FOUND_MESSAGE_TEXT[] = "You found cameleon!";
	constexpr float HUNTER_FOUND_MESSAGE_DURATION = 1.0f;
	constexpr float ROUND_RESULT_MESSAGE_DURATION = 4.0f;

	using PlayerPaintTexture = ClientGameplay::PlayerPaintTexture;
	using CameraMode = ClientGameplay::CameraMode;
	using PaintColorSlider = ClientGameplay::PaintColorSlider;
	using PaintDebugState = ClientGameplay::PaintDebugState;
	using RemotePlayerVisual = ClientGameplay::RemotePlayerVisual;

	glm::vec3 toGlmPosition(const objl::Vector3 &value)
	{
		return {value.X, value.Y, value.Z};
	}

	std::string buildMapCenterCacheKey(const GameMapDefinition &mapDefinition)
	{
		std::ostringstream key;
		key
			<< mapDefinition.modelFile << '|'
			<< mapDefinition.importScale << '|'
			<< mapDefinition.rotationDegrees.x << '|'
			<< mapDefinition.rotationDegrees.y << '|'
			<< mapDefinition.rotationDegrees.z;
		return key.str();
	}

	bool computeMapHorizontalCenter(const GameMapDefinition &mapDefinition, glm::vec3 &outCenter)
	{
		static std::map<std::string, glm::vec3> cachedCenters;

		const std::string cacheKey = buildMapCenterCacheKey(mapDefinition);
		if (auto found = cachedCenters.find(cacheKey); found != cachedCenters.end())
		{
			outCenter = found->second;
			return true;
		}

		gl3d::ErrorReporter errorReporter;
		gl3d::FileOpener fileOpener;
		gl3d::LoadedModelData loadedModel(mapDefinition.modelFile, errorReporter, fileOpener, mapDefinition.importScale);
		if (loadedModel.loader.LoadedMeshes.empty())
		{
			return false;
		}

		gl3d::Transform rotationTransform = {};
		rotationTransform.rotation = {
			glm::radians(mapDefinition.rotationDegrees.x),
			glm::radians(mapDefinition.rotationDegrees.y),
			glm::radians(mapDefinition.rotationDegrees.z)
		};
		const glm::mat4 rotationMatrix = gl3d::getTransformMatrix(rotationTransform);

		glm::vec3 minBounds(FLT_MAX);
		glm::vec3 maxBounds(-FLT_MAX);
		bool foundAnyVertex = false;

		const auto accumulateBounds = [&](const glm::vec3 &position)
		{
			const glm::vec3 transformedPosition = glm::vec3(rotationMatrix * glm::vec4(position, 1.0f));
			minBounds = glm::min(minBounds, transformedPosition);
			maxBounds = glm::max(maxBounds, transformedPosition);
			foundAnyVertex = true;
		};

		for (const auto &mesh : loadedModel.loader.LoadedMeshes)
		{
			for (const auto &vertex : mesh.Vertices)
			{
				accumulateBounds(toGlmPosition(vertex.Position));
			}

			for (const auto &vertex : mesh.VerticesAnimations)
			{
				accumulateBounds(toGlmPosition(vertex.Position));
			}
		}

		if (!foundAnyVertex)
		{
			return false;
		}

		outCenter = (minBounds + maxBounds) * 0.5f;
		cachedCenters[cacheKey] = outCenter;
		return true;
	}

	gl3d::Transform buildMapTransform(const GameMapDefinition &mapDefinition)
	{
		gl3d::Transform transform = {};
		transform.position = mapDefinition.position;
		transform.rotation = {
			glm::radians(mapDefinition.rotationDegrees.x),
			glm::radians(mapDefinition.rotationDegrees.y),
			glm::radians(mapDefinition.rotationDegrees.z)
		};

		glm::vec3 computedCenter = {};
		if (computeMapHorizontalCenter(mapDefinition, computedCenter))
		{
			transform.position.x -= computedCenter.x;
			transform.position.z -= computedCenter.z;
		}

		return transform;
	}
}

#define USE_GPU 1

#pragma region gpu
extern "C"
{
	__declspec(dllexport) unsigned long NvOptimusEnablement = USE_GPU;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = USE_GPU;
}
#pragma endregion

bool copyTextureToCpu(ClientGameplay &gameplay, gl3d::Texture texture,
	std::vector<unsigned char> &pixels, glm::ivec2 &size, int &quality);
gl3d::Texture createPaintTexture(ClientGameplay &gameplay, const PlayerPaintTexture &paintTexture);
void uploadPlayerPaintTexture(ClientGameplay &gameplay, PlayerPaintTexture &paintTexture);

glm::vec2 consumeLookDelta(glm::vec2 &lastMousePos, bool captureMouse)
{
	glm::vec2 delta = {};

	if (platform::hasFocused() && captureMouse)
	{
		platform::showMouse(false);

		glm::vec2 currentMousePos = platform::getRelMousePosition();
		constexpr float lookSpeed = 0.2f * (1.0f / 60.0f);
		delta = (lastMousePos - currentMousePos) * lookSpeed;
		lastMousePos = currentMousePos;
	}
	else
	{
		platform::showMouse(true);
		lastMousePos = platform::getRelMousePosition();
	}

	if (platform::isButtonPressed(platform::Button::Escape))
	{
		platform::showMouse(true);
		platform::loseFocus();
	}

	return delta;
}

glm::vec2 consumeMouseDragDelta(glm::vec2 &lastMousePos, bool dragActive)
{
	glm::vec2 currentMousePos = platform::getRelMousePosition();
	glm::vec2 delta = {};

	if (platform::hasFocused() && dragActive)
	{
		constexpr float lookSpeed = 0.2f * (1.0f / 60.0f);
		delta = (lastMousePos - currentMousePos) * lookSpeed;
	}

	lastMousePos = currentMousePos;
	return delta;
}

glm::vec2 consumeRawMouseDragDelta(glm::vec2 &lastMousePos, bool dragActive)
{
	glm::vec2 currentMousePos = platform::getRelMousePosition();
	glm::vec2 delta = {};

	if (platform::hasFocused() && dragActive)
	{
		delta = currentMousePos - lastMousePos;
	}

	lastMousePos = currentMousePos;
	return delta;
}

glm::vec3 hsvToRgb(glm::vec3 hsv)
{
	const float h = glm::fract(hsv.x);
	const float s = std::clamp(hsv.y, 0.0f, 1.0f);
	const float v = std::clamp(hsv.z, 0.0f, 1.0f);
	const float chroma = v * s;
	const float hueSection = h * 6.0f;
	const float x = chroma * (1.0f - std::abs(std::fmod(hueSection, 2.0f) - 1.0f));

	glm::vec3 rgb = {};
	switch (static_cast<int>(std::floor(hueSection)) % 6)
	{
	case 0: rgb = {chroma, x, 0.0f}; break;
	case 1: rgb = {x, chroma, 0.0f}; break;
	case 2: rgb = {0.0f, chroma, x}; break;
	case 3: rgb = {0.0f, x, chroma}; break;
	case 4: rgb = {x, 0.0f, chroma}; break;
	default: rgb = {chroma, 0.0f, x}; break;
	}

	const float match = v - chroma;
	return rgb + glm::vec3(match);
}

glm::vec3 rgbToHsv(glm::vec3 rgb)
{
	rgb = glm::clamp(rgb, glm::vec3(0.0f), glm::vec3(1.0f));

	const float maxChannel = (std::max)(rgb.r, (std::max)(rgb.g, rgb.b));
	const float minChannel = (std::min)(rgb.r, (std::min)(rgb.g, rgb.b));
	const float delta = maxChannel - minChannel;

	float hue = 0.0f;
	if (delta > 0.00001f)
	{
		if (maxChannel == rgb.r)
		{
			hue = std::fmod((rgb.g - rgb.b) / delta, 6.0f);
		}
		else if (maxChannel == rgb.g)
		{
			hue = ((rgb.b - rgb.r) / delta) + 2.0f;
		}
		else
		{
			hue = ((rgb.r - rgb.g) / delta) + 4.0f;
		}

		hue /= 6.0f;
		if (hue < 0.0f)
		{
			hue += 1.0f;
		}
	}

	const float saturation = maxChannel > 0.00001f ? (delta / maxChannel) : 0.0f;
	return {hue, saturation, maxChannel};
}

gl2d::Color4f toColor4f(glm::vec3 rgb, float alpha = 1.0f)
{
	return {rgb.r, rgb.g, rgb.b, alpha};
}

bool pointInRect(glm::vec2 point, gl2d::Rect rect)
{
	return point.x >= rect.x
		&& point.y >= rect.y
		&& point.x <= rect.x + rect.z
		&& point.y <= rect.y + rect.w;
}

glm::vec3 buildThirdPersonForward(float yaw, float pitch)
{
	const float cosPitch = std::cos(pitch);
	return glm::normalize(glm::vec3(
		std::sin(yaw) * cosPitch,
		std::sin(pitch),
		-std::cos(yaw) * cosPitch));
}

glm::vec3 getPlanarForward(glm::vec3 forward, glm::vec3 fallback = {0.0f, 0.0f, -1.0f})
{
	forward.y = 0.0f;
	const float forwardLength = glm::length(forward);

	if (forwardLength < 0.0001f)
	{
		return fallback;
	}

	return forward / forwardLength;
}

glm::vec3 getRightFromForward(glm::vec3 forward)
{
	glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
	const float rightLength = glm::length(right);

	if (rightLength < 0.0001f)
	{
		return {1.0f, 0.0f, 0.0f};
	}

	return right / rightLength;
}

float getPlayerVisualScale(const ClientGameplay &gameplay, std::uint64_t cid)
{
	return gameplay.clientNetworking.isHunter(cid) ? HUNTER_VISUAL_SCALE : 1.0f;
}

float getLocalPlayerVisualScale(const ClientGameplay &gameplay)
{
	return getPlayerVisualScale(gameplay, gameplay.clientNetworking.localCID);
}

float getThirdPersonTargetHeight(const ClientGameplay &gameplay)
{
	return THIRD_PERSON_CAMERA_TARGET_HEIGHT * getLocalPlayerVisualScale(gameplay);
}

bool isHunterGameplayCameraLocked(const ClientGameplay &gameplay)
{
	return gameplay.clientNetworking.gameActive && gameplay.clientNetworking.isLocalHunter();
}

bool isHunterHidePhaseActive(const ClientGameplay &gameplay)
{
	return gameplay.clientNetworking.gameActive
		&& gameplay.clientNetworking.isLocalHunter()
		&& gameplay.clientNetworking.roundPhase == ClientNetworking::RoundPhase::HiderHide;
}

bool isHunterFirstPersonActive(const ClientGameplay &gameplay)
{
	return gameplay.cameraMode == CameraMode::ThirdPerson
		&& gameplay.clientNetworking.gameActive
		&& gameplay.clientNetworking.isLocalHunter()
		&& gameplay.clientNetworking.roundPhase == ClientNetworking::RoundPhase::HunterSearch
		&& !gameplay.paintModeActive;
}

void syncThirdPersonOrbitToCamera(ClientGameplay &gameplay)
{
	auto &renderer3D = gameplay.renderer3D;
	auto &thirdPersonPitch = gameplay.thirdPersonPitch;
	auto &thirdPersonYaw = gameplay.thirdPersonYaw;

	const glm::vec3 viewDirection = glm::normalize(renderer3D.camera.viewDirection);
	thirdPersonPitch = std::asin(std::clamp(viewDirection.y, -1.0f, 1.0f));
	thirdPersonYaw = std::atan2(viewDirection.x, -viewDirection.z);
}

void setCameraMode(ClientGameplay &gameplay, CameraMode newMode)
{
	auto &cameraMode = gameplay.cameraMode;
	auto &paintModeActive = gameplay.paintModeActive;
	auto &paintColorPickModeActive = gameplay.paintColorPickModeActive;

	if (newMode != CameraMode::ThirdPerson && isHunterGameplayCameraLocked(gameplay))
	{
		return;
	}

	if (cameraMode == newMode)
	{
		return;
	}

	if (newMode == CameraMode::ThirdPerson)
	{
		syncThirdPersonOrbitToCamera(gameplay);
	}
	else
	{
		paintModeActive = false;
		paintColorPickModeActive = false;
	}

	cameraMode = newMode;
}

void toggleCameraMode(ClientGameplay &gameplay)
{
	setCameraMode(gameplay, gameplay.cameraMode == CameraMode::Free ? CameraMode::ThirdPerson : CameraMode::Free);
}

void updateForcedHunterHidePaintModeState(ClientGameplay &gameplay)
{
	if (isHunterHidePhaseActive(gameplay))
	{
		gameplay.paintModeActive = true;
		gameplay.hunterHidePhaseForcedPaintMode = true;
		return;
	}

	if (gameplay.hunterHidePhaseForcedPaintMode)
	{
		gameplay.hunterHidePhaseForcedPaintMode = false;
		gameplay.paintModeActive = false;
		gameplay.paintColorPickModeActive = false;
		gameplay.hasLastPaintStrokeScreenPosition = false;
	}
}

void applyFreeCameraInput(::gl3d::Renderer3D &rendererRef, float speed, float deltaTime, const glm::vec2 &lookDelta)
{
	glm::vec3 dir = {};
	if (platform::isButtonHeld(platform::Button::W))
	{
		dir.z -= speed * deltaTime;
	}
	if (platform::isButtonHeld(platform::Button::S))
	{
		dir.z += speed * deltaTime;
	}

	if (platform::isButtonHeld(platform::Button::A))
	{
		dir.x -= speed * deltaTime;
	}
	if (platform::isButtonHeld(platform::Button::D))
	{
		dir.x += speed * deltaTime;
	}

	if (platform::isButtonHeld(platform::Button::Q))
	{
		dir.y -= speed * deltaTime;
	}
	if (platform::isButtonHeld(platform::Button::E))
	{
		dir.y += speed * deltaTime;
	}

	rendererRef.camera.moveFPS(dir);
	rendererRef.camera.rotateCamera(lookDelta);
}

PhysicsControllerInput buildPlayerInput(ClientGameplay &gameplay)
{
	auto &thirdPersonYaw = gameplay.thirdPersonYaw;

	PhysicsControllerInput result = {};

	const glm::vec3 planarForward = getPlanarForward(
		buildThirdPersonForward(thirdPersonYaw, 0.0f));
	const glm::vec3 right = getRightFromForward(planarForward);

	if (platform::isButtonHeld(platform::Button::W))
	{
		result.moveDirection += planarForward;
	}
	if (platform::isButtonHeld(platform::Button::S))
	{
		result.moveDirection -= planarForward;
	}
	if (platform::isButtonHeld(platform::Button::D))
	{
		result.moveDirection += right;
	}
	if (platform::isButtonHeld(platform::Button::A))
	{
		result.moveDirection -= right;
	}

	if (glm::length(result.moveDirection) > 0.0001f)
	{
		result.moveDirection = glm::normalize(result.moveDirection);
	}

	result.wantsToRun = platform::isButtonHeld(platform::Button::LeftShift);
	result.wantsToJump = platform::isButtonPressed(platform::Button::Space);
	result.wantsToClimb = platform::isButtonHeld(platform::Button::Space);
	return result;
}

int wrapAnimationIndex(int animationIndex)
{
	if (animationIndex < 0)
	{
		animationIndex = PLAYER_ANIMATIONS - 1;
	}

	return animationIndex % PLAYER_ANIMATIONS;
}

void updateLocalAnimationSelection(ClientGameplay &gameplay)
{
	auto &cameraMode = gameplay.cameraMode;
	auto &paintModeActive = gameplay.paintModeActive;
	auto &localAnimationIndex = gameplay.localAnimationIndex;

	if (cameraMode != CameraMode::ThirdPerson || paintModeActive)
	{
		return;
	}

	if (platform::isButtonPressed(platform::Button::Q))
	{
		localAnimationIndex = wrapAnimationIndex(localAnimationIndex - 1);
	}

	if (platform::isButtonPressed(platform::Button::E))
	{
		localAnimationIndex = wrapAnimationIndex(localAnimationIndex + 1);
	}
}

int getCurrentLocalAnimationIndex(ClientGameplay &gameplay, const PhysicsControllerInput &playerInput)
{
	const bool hasMovementInput = glm::dot(playerInput.moveDirection, playerInput.moveDirection) > 0.0001f;
	if (playerInput.wantsToRun && hasMovementInput)
	{
		gameplay.localAnimationIndex = 9;
	}

	return gameplay.localAnimationIndex;
}

void syncPlayerEntityToPhysics(ClientGameplay &gameplay)
{
	auto &renderer3D = gameplay.renderer3D;
	auto &playerEntity = gameplay.playerEntity;
	auto &playerPhysics = gameplay.playerPhysics;

	gl3d::Transform playerTransform = renderer3D.getEntityTransform(playerEntity);
	playerTransform.position = playerPhysics.getPlayerPosition();
	playerTransform.rotation = {};
	playerTransform.rotation.y = playerPhysics.getPlayerYaw() + PLAYER_MODEL_YAW_OFFSET;
	playerTransform.scale = glm::vec3(getLocalPlayerVisualScale(gameplay));
	renderer3D.setEntityTransform(playerEntity, playerTransform);
}

Packet_PlayerStateUpdate buildLocalPlayerState(ClientGameplay &gameplay, int animationIndex)
{
	Packet_PlayerStateUpdate playerState = {};
	playerState.position = gameplay.playerPhysics.getPlayerPosition();
	playerState.yaw = gameplay.playerPhysics.getPlayerYaw();
	playerState.animationIndex = animationIndex;
	return playerState;
}

bool didPlayerStateChange(const Packet_PlayerStateUpdate &currentState,
	const Packet_PlayerStateUpdate &previousState)
{
	const glm::vec3 positionDelta = currentState.position - previousState.position;
	if (glm::dot(positionDelta, positionDelta) > PLAYER_STATE_POSITION_EPSILON * PLAYER_STATE_POSITION_EPSILON)
	{
		return true;
	}

	if (std::abs(currentState.yaw - previousState.yaw) > PLAYER_STATE_YAW_EPSILON)
	{
		return true;
	}

	return currentState.animationIndex != previousState.animationIndex;
}

float normalizeAngle(float angle)
{
	const float twoPi = glm::two_pi<float>();

	while (angle > glm::pi<float>())
	{
		angle -= twoPi;
	}

	while (angle < -glm::pi<float>())
	{
		angle += twoPi;
	}

	return angle;
}

float interpolateAngle(float currentAngle, float targetAngle, float alpha)
{
	const float delta = normalizeAngle(targetAngle - currentAngle);
	return normalizeAngle(currentAngle + delta * alpha);
}

void clearPaintTextures(ClientGameplay &gameplay, std::vector<PlayerPaintTexture> &paintTextures)
{
	auto &renderer3D = gameplay.renderer3D;

	for (auto &paintTexture : paintTextures)
	{
		if (renderer3D.isTexture(paintTexture.texture))
		{
			renderer3D.deleteTexture(paintTexture.texture);
		}
	}

	paintTextures.clear();
}

PlayerPaintTexture *getPaintTexture(std::vector<PlayerPaintTexture> &paintTextures, int meshIndex)
{
	for (auto &paintTexture : paintTextures)
	{
		if (paintTexture.meshIndex == meshIndex)
		{
			return &paintTexture;
		}
	}

	return nullptr;
}

void setupEntityPaintTextures(ClientGameplay &gameplay, gl3d::Entity &entity,
	std::vector<PlayerPaintTexture> &paintTextures, bool setAsPaintTarget)
{
	auto &renderer3D = gameplay.renderer3D;

	clearPaintTextures(gameplay, paintTextures);

	const int meshCount = renderer3D.getEntityMeshesCount(entity);
	for (int meshIndex = 0; meshIndex < meshCount; ++meshIndex)
	{
		gl3d::TextureDataForMaterial materialTextures = renderer3D.getEntityMeshMaterialTextures(entity, meshIndex);
		if (!renderer3D.isTexture(materialTextures.albedoTexture))
		{
			continue;
		}

		PlayerPaintTexture paintTexture = {};
		paintTexture.meshIndex = meshIndex;
		if (!copyTextureToCpu(gameplay, materialTextures.albedoTexture, paintTexture.pixels, paintTexture.size, paintTexture.quality))
		{
			continue;
		}

		paintTexture.texture = createPaintTexture(gameplay, paintTexture);
		materialTextures.albedoTexture = paintTexture.texture;
		renderer3D.setEntityMeshMaterialTextures(entity, meshIndex, materialTextures);
		paintTextures.push_back(std::move(paintTexture));
	}

	if (setAsPaintTarget)
	{
		renderer3D.setEntityPaintTarget(entity);
	}
}

void clearRemotePlayers(ClientGameplay &gameplay)
{
	auto &renderer3D = gameplay.renderer3D;
	auto &remotePlayers = gameplay.remotePlayers;

	for (auto &[cid, remotePlayer] : remotePlayers)
	{
		(void)cid;
		clearPaintTextures(gameplay, remotePlayer.paintTextures);
		if (renderer3D.isEntity(remotePlayer.entity))
		{
			renderer3D.deleteEntity(remotePlayer.entity);
		}
	}

	remotePlayers.clear();
}

void applyRemotePaintTextureUpdate(ClientGameplay &gameplay, gl3d::Entity &entity,
	std::vector<PlayerPaintTexture> &paintTextures,
	const ClientNetworking::RemotePaintTextureUpdate &update)
{
	auto &renderer3D = gameplay.renderer3D;

	PlayerPaintTexture *paintTexture = getPaintTexture(paintTextures, update.meshIndex);
	if (paintTexture == nullptr)
	{
		return;
	}

	const bool needsRecreateTexture =
		paintTexture->size != update.size
		|| paintTexture->quality != update.quality
		|| paintTexture->pixels.size() != update.pixels.size()
		|| !renderer3D.isTexture(paintTexture->texture);

	paintTexture->size = update.size;
	paintTexture->quality = update.quality;
	paintTexture->pixels = update.pixels;

	if (needsRecreateTexture)
	{
		if (renderer3D.isTexture(paintTexture->texture))
		{
			renderer3D.deleteTexture(paintTexture->texture);
		}

		paintTexture->texture = createPaintTexture(gameplay, *paintTexture);
		gl3d::TextureDataForMaterial materialTextures = renderer3D.getEntityMeshMaterialTextures(entity, update.meshIndex);
		materialTextures.albedoTexture = paintTexture->texture;
		renderer3D.setEntityMeshMaterialTextures(entity, update.meshIndex, materialTextures);
	}
	else
	{
		uploadPlayerPaintTexture(gameplay, *paintTexture);
	}
}

void applyPendingRemotePaintUpdates(ClientGameplay &gameplay, std::uint64_t cid, RemotePlayerVisual &remotePlayer)
{
	auto &clientNetworking = gameplay.clientNetworking;

	auto pendingUpdates = clientNetworking.remotePaintUpdates.find(cid);
	if (pendingUpdates == clientNetworking.remotePaintUpdates.end())
	{
		return;
	}

	for (auto &[meshIndex, update] : pendingUpdates->second)
	{
		(void)meshIndex;
		applyRemotePaintTextureUpdate(gameplay, remotePlayer.entity, remotePlayer.paintTextures, update);
	}

	clientNetworking.remotePaintUpdates.erase(pendingUpdates);
}

void syncRemotePlayers(ClientGameplay &gameplay, float deltaTime)
{
	auto &clientNetworking = gameplay.clientNetworking;
	auto &playerModel = gameplay.playerModel;
	auto &remotePlayers = gameplay.remotePlayers;
	auto &renderer3D = gameplay.renderer3D;
	const float interpolationAlpha = 1.0f - std::exp(-REMOTE_PLAYER_INTERPOLATION_SPEED * deltaTime);

	for (auto it = remotePlayers.begin(); it != remotePlayers.end();)
	{
		if (clientNetworking.remotePlayers.find(it->first) == clientNetworking.remotePlayers.end())
		{
			clearPaintTextures(gameplay, it->second.paintTextures);
			if (renderer3D.isEntity(it->second.entity))
			{
				renderer3D.deleteEntity(it->second.entity);
			}
			it = remotePlayers.erase(it);
		}
		else
		{
			++it;
		}
	}

	for (auto &[cid, remoteState] : clientNetworking.remotePlayers)
	{
		auto &remotePlayer = remotePlayers[cid];
		if (!renderer3D.isEntity(remotePlayer.entity))
		{
			remotePlayer.entity = renderer3D.createEntity(playerModel, {}, false, true, false);
			renderer3D.setEntityAnimate(remotePlayer.entity, true);
			setupEntityPaintTextures(gameplay, remotePlayer.entity, remotePlayer.paintTextures, false);
		}

		if (!remotePlayer.hasVisualState)
		{
			remotePlayer.visualPosition = remoteState.position;
			remotePlayer.visualYaw = remoteState.yaw;
			remotePlayer.hasVisualState = true;
		}
		else
		{
			const glm::vec3 positionDelta = remoteState.position - remotePlayer.visualPosition;
			const float distanceToTarget = glm::length(positionDelta);

			if (distanceToTarget > REMOTE_PLAYER_SNAP_DISTANCE)
			{
				remotePlayer.visualPosition = remoteState.position;
				remotePlayer.visualYaw = remoteState.yaw;
			}
			else
			{
				remotePlayer.visualPosition = glm::mix(
					remotePlayer.visualPosition,
					remoteState.position,
					interpolationAlpha);
				remotePlayer.visualYaw = interpolateAngle(
					remotePlayer.visualYaw,
					remoteState.yaw,
					interpolationAlpha);
			}
		}

		gl3d::Transform transform = renderer3D.getEntityTransform(remotePlayer.entity);
		transform.position = remotePlayer.visualPosition;
		transform.rotation = {};
		transform.rotation.y = remotePlayer.visualYaw + PLAYER_MODEL_YAW_OFFSET;
		transform.scale = glm::vec3(getPlayerVisualScale(gameplay, cid));
		renderer3D.setEntityTransform(remotePlayer.entity, transform);
		renderer3D.setEntityAnimationIndex(remotePlayer.entity, remoteState.animationIndex);
		renderer3D.setEntityVisible(remotePlayer.entity, !isHunterHidePhaseActive(gameplay));
		applyPendingRemotePaintUpdates(gameplay, cid, remotePlayer);
	}
}

void sendLocalPlayerState(ClientGameplay &gameplay, float deltaTime, int animationIndex)
{
	auto &clientNetworking = gameplay.clientNetworking;
	auto &hasLastSentPlayerState = gameplay.hasLastSentPlayerState;
	auto &lastSentPlayerState = gameplay.lastSentPlayerState;
	auto &timeSinceLastReliablePlayerStateSent = gameplay.timeSinceLastReliablePlayerStateSent;
	auto &timeSinceLastUnreliablePlayerStateSent = gameplay.timeSinceLastUnreliablePlayerStateSent;

	if (!clientNetworking.receivedPlayerData || clientNetworking.localCID == 0)
	{
		return;
	}

	timeSinceLastReliablePlayerStateSent += deltaTime;
	timeSinceLastUnreliablePlayerStateSent += deltaTime;

	const Packet_PlayerStateUpdate currentState = buildLocalPlayerState(gameplay, animationIndex);
	const bool stateChanged = !hasLastSentPlayerState || didPlayerStateChange(currentState, lastSentPlayerState);

	if (timeSinceLastReliablePlayerStateSent >= PLAYER_STATE_SEND_INTERVAL_RELIABLE)
	{
		if (clientNetworking.sendPlayerState(currentState, true))
		{
			timeSinceLastReliablePlayerStateSent = 0.0f;
			timeSinceLastUnreliablePlayerStateSent = 0.0f;
			lastSentPlayerState = currentState;
			hasLastSentPlayerState = true;
		}
		return;
	}

	if (stateChanged && timeSinceLastUnreliablePlayerStateSent >= PLAYER_STATE_SEND_INTERVAL_UNRELIABLE)
	{
		if (clientNetworking.sendPlayerState(currentState, false))
		{
			timeSinceLastUnreliablePlayerStateSent = 0.0f;
			lastSentPlayerState = currentState;
			hasLastSentPlayerState = true;
		}
	}
}

void sendLocalPaintTextures(ClientGameplay &gameplay, float deltaTime)
{
	auto &clientNetworking = gameplay.clientNetworking;
	auto &localPaintTexturesDirty = gameplay.localPaintTexturesDirty;
	auto &playerPaintTextures = gameplay.playerPaintTextures;
	auto &timeSinceLastPaintTextureSync = gameplay.timeSinceLastPaintTextureSync;

	if (!clientNetworking.receivedPlayerData || clientNetworking.localCID == 0)
	{
		return;
	}

	if (!localPaintTexturesDirty)
	{
		timeSinceLastPaintTextureSync = 0.0f;
		return;
	}

	timeSinceLastPaintTextureSync += deltaTime;
	if (timeSinceLastPaintTextureSync < PLAYER_PAINT_TEXTURE_SEND_INTERVAL)
	{
		return;
	}

	bool anyTextureStillDirty = false;
	for (auto &paintTexture : playerPaintTextures)
	{
		if (!paintTexture.networkDirty)
		{
			continue;
		}

		if (clientNetworking.sendPaintTextureUpdate(
			paintTexture.meshIndex,
			paintTexture.size,
			paintTexture.quality,
			paintTexture.pixels))
		{
			paintTexture.networkDirty = false;
		}
		else
		{
			anyTextureStillDirty = true;
		}
	}

	for (auto &paintTexture : playerPaintTextures)
	{
		if (paintTexture.networkDirty)
		{
			anyTextureStillDirty = true;
			break;
		}
	}

	localPaintTexturesDirty = anyTextureStillDirty;
	timeSinceLastPaintTextureSync = localPaintTexturesDirty ? PLAYER_PAINT_TEXTURE_SEND_INTERVAL : 0.0f;
}

bool copyTextureToCpu(ClientGameplay &gameplay, gl3d::Texture texture, std::vector<unsigned char> &pixels, glm::ivec2 &size, int &quality)
{
	auto &renderer3D = gameplay.renderer3D;

	gl3d::GpuTexture *gpuTexture = renderer3D.getTextureData(texture);
	if (gpuTexture == nullptr || gpuTexture->id == 0)
	{
		return false;
	}

	size = gpuTexture->getTextureSize();
	if (size.x <= 0 || size.y <= 0)
	{
		return false;
	}

	quality = gpuTexture->getTextureQuality();
	pixels.assign(size.x * size.y * 4, 255);

	glBindTexture(GL_TEXTURE_2D, gpuTexture->id);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	return true;
}

void computeTextureAlphaFlags(const std::vector<unsigned char> &pixels, bool &alphaExists, bool &alphaHasData)
{
	alphaExists = false;
	alphaHasData = false;

	for (size_t i = 3; i < pixels.size(); i += 4)
	{
		if (pixels[i] != 255)
		{
			alphaExists = true;
		}

		if (pixels[i] != 0 && pixels[i] != 255)
		{
			alphaHasData = true;
		}

		if (alphaExists && alphaHasData)
		{
			return;
		}
	}
}

gl3d::Texture createPaintTexture(ClientGameplay &gameplay, const PlayerPaintTexture &paintTexture)
{
	auto &renderer3D = gameplay.renderer3D;

	bool alphaExists = false;
	bool alphaHasData = false;
	computeTextureAlphaFlags(paintTexture.pixels, alphaExists, alphaHasData);

	gl3d::GpuTexture gpuTexture;
	gpuTexture.loadTextureFromMemory(
		const_cast<unsigned char *>(paintTexture.pixels.data()),
		paintTexture.size.x,
		paintTexture.size.y,
		4,
		paintTexture.quality);

	return renderer3D.createIntenralTexture(
		gpuTexture,
		alphaExists ? 1 : 0,
		alphaHasData ? 1 : 0,
		"playerPaintMesh" + std::to_string(paintTexture.meshIndex));
}

PlayerPaintTexture *getPlayerPaintTexture(ClientGameplay &gameplay, int meshIndex)
{
	return getPaintTexture(gameplay.playerPaintTextures, meshIndex);
}

bool isPaintBrushResizeActive(ClientGameplay &gameplay, const platform::Input &input)
{
	auto &cameraMode = gameplay.cameraMode;
	auto &paintModeActive = gameplay.paintModeActive;
	auto &paintColorUiHovered = gameplay.paintColorUiHovered;

	return cameraMode == CameraMode::ThirdPerson
		&& paintModeActive
		&& !paintColorUiHovered
		&& input.rMouse.held;
}

glm::ivec2 getMouseFramebufferPosition(ClientGameplay &gameplay, const platform::Input &input);

gl2d::Rect getPaintColorPanelRect()
{
	return {
		PAINT_COLOR_PANEL_MARGIN,
		PAINT_COLOR_PANEL_MARGIN,
		PAINT_COLOR_PANEL_WIDTH,
		PAINT_COLOR_PANEL_HEIGHT
	};
}

gl2d::Rect getPaintColorPickButtonRect()
{
	const gl2d::Rect panel = getPaintColorPanelRect();
	return {
		panel.x + 84.0f,
		panel.y + panel.w - 30.0f,
		124.0f,
		22.0f
	};
}

gl2d::Rect getPaintColorSliderRect(PaintColorSlider slider)
{
	const gl2d::Rect panel = getPaintColorPanelRect();
	const float sliderX = panel.x + 84.0f;
	const float sliderYBase = panel.y + 46.0f;
	const float sliderSpacing = 34.0f;
	float row = 0.0f;

	switch (slider)
	{
	case PaintColorSlider::Hue: row = 0.0f; break;
	case PaintColorSlider::Saturation: row = 1.0f; break;
	case PaintColorSlider::Value: row = 2.0f; break;
	default: return {};
	}

	return {
		sliderX,
		sliderYBase + row * sliderSpacing,
		PAINT_COLOR_SLIDER_WIDTH,
		PAINT_COLOR_SLIDER_HEIGHT
	};
}

float *getPaintColorSliderValue(ClientGameplay &gameplay, PaintColorSlider slider)
{
	auto &playerPaintColorHsv = gameplay.playerPaintColorHsv;

	switch (slider)
	{
	case PaintColorSlider::Hue: return &playerPaintColorHsv.x;
	case PaintColorSlider::Saturation: return &playerPaintColorHsv.y;
	case PaintColorSlider::Value: return &playerPaintColorHsv.z;
	default: return nullptr;
	}
}

void setPaintColorSliderValue(ClientGameplay &gameplay, PaintColorSlider slider, float normalizedValue)
{
	float *value = getPaintColorSliderValue(gameplay, slider);
	if (value == nullptr)
	{
		return;
	}

	*value = std::clamp(normalizedValue, 0.0f, 1.0f);
}

void updatePaintColorPicker(ClientGameplay &gameplay, platform::Input &input)
{
	auto &paintColorUiHovered = gameplay.paintColorUiHovered;
	auto &paintColorUiCapturingMouse = gameplay.paintColorUiCapturingMouse;
	auto &paintModeActive = gameplay.paintModeActive;
	auto &cameraMode = gameplay.cameraMode;
	auto &activePaintColorSlider = gameplay.activePaintColorSlider;
	auto &paintColorPickModeActive = gameplay.paintColorPickModeActive;

	paintColorUiHovered = false;
	paintColorUiCapturingMouse = false;

	if (!paintModeActive || cameraMode != CameraMode::ThirdPerson)
	{
		activePaintColorSlider = PaintColorSlider::None;
		paintColorPickModeActive = false;
		return;
	}

	const glm::vec2 mousePosition = glm::vec2(getMouseFramebufferPosition(gameplay, input));
	const gl2d::Rect panel = getPaintColorPanelRect();
	paintColorUiHovered = pointInRect(mousePosition, panel);

	if (platform::isButtonPressed(platform::Button::C))
	{
		paintColorPickModeActive = !paintColorPickModeActive;
		activePaintColorSlider = PaintColorSlider::None;
	}

	if (!input.isLMouseHeld())
	{
		activePaintColorSlider = PaintColorSlider::None;
	}

	if (input.isLMousePressed())
	{
		if (pointInRect(mousePosition, getPaintColorPickButtonRect()))
		{
			paintColorPickModeActive = !paintColorPickModeActive;
			activePaintColorSlider = PaintColorSlider::None;
		}

		for (PaintColorSlider slider : {PaintColorSlider::Hue, PaintColorSlider::Saturation, PaintColorSlider::Value})
		{
			if (pointInRect(mousePosition, getPaintColorSliderRect(slider)))
			{
				paintColorPickModeActive = false;
				activePaintColorSlider = slider;
				break;
			}
		}
	}

	if (activePaintColorSlider != PaintColorSlider::None && input.isLMouseHeld())
	{
		paintColorPickModeActive = false;
		const gl2d::Rect sliderRect = getPaintColorSliderRect(activePaintColorSlider);
		const float sliderValue = (mousePosition.x - sliderRect.x) / sliderRect.z;
		setPaintColorSliderValue(gameplay, activePaintColorSlider, sliderValue);
	}

	paintColorUiCapturingMouse = activePaintColorSlider != PaintColorSlider::None
		|| (paintColorUiHovered && input.isLMouseHeld());
}

glm::vec3 getPaintColorSliderPreview(ClientGameplay &gameplay, PaintColorSlider slider, float value)
{
	auto &playerPaintColorHsv = gameplay.playerPaintColorHsv;

	switch (slider)
	{
	case PaintColorSlider::Hue:
		return hsvToRgb({value, 1.0f, 1.0f});
	case PaintColorSlider::Saturation:
		return hsvToRgb({playerPaintColorHsv.x, value, (std::max)(playerPaintColorHsv.z, 1.0f)});
	case PaintColorSlider::Value:
		return hsvToRgb({playerPaintColorHsv.x, playerPaintColorHsv.y, value});
	default:
		return {};
	}
}

glm::ivec2 getMouseFramebufferPosition(ClientGameplay &gameplay, const platform::Input &input)
{
	auto &paintDebugState = gameplay.paintDebugState;

	const glm::ivec2 windowSize = platform::getWindowSize();
	const glm::ivec2 framebufferSize = platform::getFrameBufferSize();

	paintDebugState.windowMousePosition = {input.mouseX, input.mouseY};

	if (windowSize.x <= 0 || windowSize.y <= 0)
	{
		paintDebugState.framebufferMousePosition = paintDebugState.windowMousePosition;
		return paintDebugState.framebufferMousePosition;
	}

	paintDebugState.framebufferMousePosition = {
		input.mouseX * framebufferSize.x / windowSize.x,
		input.mouseY * framebufferSize.y / windowSize.y
	};

	return paintDebugState.framebufferMousePosition;
}

bool sampleVisibleScreenColor(glm::ivec2 screenPosition, glm::vec3 &sampledColor)
{
	sampledColor = {};
	constexpr int sampleRadius = 2;

	const glm::ivec2 framebufferSize = platform::getFrameBufferSize();
	if (framebufferSize.x <= 0 || framebufferSize.y <= 0)
	{
		return false;
	}

	if (screenPosition.x < 0 || screenPosition.y < 0
		|| screenPosition.x >= framebufferSize.x || screenPosition.y >= framebufferSize.y)
	{
		return false;
	}

	const int minX = (std::max)(0, screenPosition.x - sampleRadius);
	const int maxX = (std::min)(framebufferSize.x - 1, screenPosition.x + sampleRadius);
	const int minY = (std::max)(0, screenPosition.y - sampleRadius);
	const int maxY = (std::min)(framebufferSize.y - 1, screenPosition.y + sampleRadius);
	const int sampleWidth = maxX - minX + 1;
	const int sampleHeight = maxY - minY + 1;
	const GLint readY = framebufferSize.y - 1 - maxY;
	GLint previousReadFramebuffer = 0;
	GLint previousReadBuffer = GL_BACK;
	GLint previousPackAlignment = 4;

	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);

	std::vector<unsigned char> pixels(static_cast<size_t>(sampleWidth * sampleHeight) * 4);
	glReadPixels(minX, readY, sampleWidth, sampleHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

	glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
	glReadBuffer(static_cast<GLenum>(previousReadBuffer));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);

	glm::vec3 accumulatedColor = {};
	const size_t pixelCount = static_cast<size_t>(sampleWidth * sampleHeight);
	for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
	{
		const size_t baseIndex = pixelIndex * 4;
		accumulatedColor.r += pixels[baseIndex + 0] / 255.0f;
		accumulatedColor.g += pixels[baseIndex + 1] / 255.0f;
		accumulatedColor.b += pixels[baseIndex + 2] / 255.0f;
	}

	sampledColor = accumulatedColor / static_cast<float>(pixelCount);

	return true;
}

void uploadPlayerPaintTexture(ClientGameplay &gameplay, PlayerPaintTexture &paintTexture)
{
	auto &renderer3D = gameplay.renderer3D;

	gl3d::GpuTexture *gpuTexture = renderer3D.getTextureData(paintTexture.texture);
	if (gpuTexture == nullptr || gpuTexture->id == 0)
	{
		return;
	}

	glBindTexture(GL_TEXTURE_2D, gpuTexture->id);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(
		GL_TEXTURE_2D,
		0,
		0,
		0,
		paintTexture.size.x,
		paintTexture.size.y,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		paintTexture.pixels.data());

	if (paintTexture.quality > gl3d::linearNoMipmap)
	{
		glGenerateMipmap(GL_TEXTURE_2D);
	}
}

void paintTextureColor(ClientGameplay &gameplay, PlayerPaintTexture &paintTexture, glm::ivec2 center, glm::vec3 color)
{
	auto &playerPaintBrushRadius = gameplay.playerPaintBrushRadius;

	const unsigned char red = static_cast<unsigned char>(std::clamp(color.r * 255.0f, 0.0f, 255.0f));
	const unsigned char green = static_cast<unsigned char>(std::clamp(color.g * 255.0f, 0.0f, 255.0f));
	const unsigned char blue = static_cast<unsigned char>(std::clamp(color.b * 255.0f, 0.0f, 255.0f));

	for (int y = center.y - playerPaintBrushRadius; y <= center.y + playerPaintBrushRadius; ++y)
	{
		for (int x = center.x - playerPaintBrushRadius; x <= center.x + playerPaintBrushRadius; ++x)
		{
			if (x < 0 || y < 0 || x >= paintTexture.size.x || y >= paintTexture.size.y)
			{
				continue;
			}

			const glm::ivec2 delta = glm::ivec2(x, y) - center;
			if (delta.x * delta.x + delta.y * delta.y > playerPaintBrushRadius * playerPaintBrushRadius)
			{
				continue;
			}

			const size_t pixelIndex = static_cast<size_t>(x + y * paintTexture.size.x) * 4;
			paintTexture.pixels[pixelIndex + 0] = red;
			paintTexture.pixels[pixelIndex + 1] = green;
			paintTexture.pixels[pixelIndex + 2] = blue;
		}
	}
}

void resetPaintStrokeState(ClientGameplay &gameplay)
{
	auto &hasLastPaintStrokeScreenPosition = gameplay.hasLastPaintStrokeScreenPosition;
	auto &lastPaintStrokeScreenPosition = gameplay.lastPaintStrokeScreenPosition;

	hasLastPaintStrokeScreenPosition = false;
	lastPaintStrokeScreenPosition = {};
}

void resetEntityPaintTexturesToModelDefaults(ClientGameplay &gameplay, gl3d::Entity &entity,
	gl3d::Model &model, std::vector<PlayerPaintTexture> &paintTextures, bool setAsPaintTarget)
{
	auto &renderer3D = gameplay.renderer3D;

	clearPaintTextures(gameplay, paintTextures);
	renderer3D.setEntityModel(entity, model);
	renderer3D.setEntityAnimate(entity, true);
	setupEntityPaintTextures(gameplay, entity, paintTextures, setAsPaintTarget);
}

void resetClientPositionsToSpawn(ClientGameplay &gameplay)
{
	auto &clientNetworking = gameplay.clientNetworking;
	auto &playerPhysics = gameplay.playerPhysics;
	auto &remotePlayers = gameplay.remotePlayers;

	playerPhysics.resetToSpawn();
	const glm::vec3 spawnPosition = playerPhysics.getSpawnPosition();

	for (auto &[cid, remoteState] : clientNetworking.remotePlayers)
	{
		(void)cid;
		remoteState.position = spawnPosition;
		remoteState.yaw = 0.0f;
		remoteState.animationIndex = 9;
	}

	for (auto &[cid, remotePlayer] : remotePlayers)
	{
		(void)cid;
		remotePlayer.visualPosition = spawnPosition;
		remotePlayer.visualYaw = 0.0f;
		remotePlayer.hasVisualState = true;
	}

	gameplay.localAnimationIndex = 9;
	gameplay.hasLastSentPlayerState = false;
	gameplay.lastSentPlayerState = {};
	gameplay.timeSinceLastReliablePlayerStateSent = PLAYER_STATE_SEND_INTERVAL_RELIABLE;
	gameplay.timeSinceLastUnreliablePlayerStateSent = 0.0f;
	gameplay.timeSinceLastPaintTextureSync = 0.0f;
	gameplay.localPaintTexturesDirty = false;
	gameplay.paintColorPickModeActive = false;
	gameplay.paintDebugState = {};
	resetPaintStrokeState(gameplay);
}

void resetClientRoundStartState(ClientGameplay &gameplay)
{
	auto &clientNetworking = gameplay.clientNetworking;
	auto &playerEntity = gameplay.playerEntity;
	auto &playerModel = gameplay.playerModel;
	auto &playerPaintTextures = gameplay.playerPaintTextures;
	auto &remotePlayers = gameplay.remotePlayers;

	resetClientPositionsToSpawn(gameplay);
	resetEntityPaintTexturesToModelDefaults(
		gameplay,
		playerEntity,
		playerModel,
		playerPaintTextures,
		true);

	clientNetworking.remotePaintUpdates.clear();
	for (auto &[cid, remotePlayer] : remotePlayers)
	{
		(void)cid;
		if (gameplay.renderer3D.isEntity(remotePlayer.entity))
		{
			resetEntityPaintTexturesToModelDefaults(
				gameplay,
				remotePlayer.entity,
				playerModel,
				remotePlayer.paintTextures,
				false);
		}
	}
}

bool loadMapByIndex(ClientGameplay &gameplay, std::uint32_t mapIndex, bool resetPlayersToSpawn)
{
	auto &renderer3D = gameplay.renderer3D;
	auto &mapEntity = gameplay.mapEntity;
	auto &mapModel = gameplay.mapModel;
	auto &loadedMapModels = gameplay.loadedMapModels;
	auto &playerPhysics = gameplay.playerPhysics;

	const std::uint32_t resolvedMapIndex = clampGameMapIndex(mapIndex);
	const GameMapDefinition &mapDefinition = getGameMapDefinition(resolvedMapIndex);
	const gl3d::Transform mapTransform = buildMapTransform(mapDefinition);

	if (renderer3D.isEntity(mapEntity))
	{
		renderer3D.deleteEntity(mapEntity);
	}

	if (loadedMapModels.size() < getGameMapCount())
	{
		loadedMapModels.resize(getGameMapCount());
	}

	gl3d::Model &cachedMapModel = loadedMapModels[resolvedMapIndex];
	if (!renderer3D.isModel(cachedMapModel))
	{
		cachedMapModel = renderer3D.loadModel(mapDefinition.modelFile, gl3d::maxQuality, mapDefinition.importScale);
	}

	mapModel = cachedMapModel;
	if (!renderer3D.isModel(mapModel))
	{
		return false;
	}

	mapEntity = renderer3D.createEntity(mapModel, mapTransform, true, true, false);
	if (!playerPhysics.setStaticMapCollision(mapDefinition.modelFile, mapDefinition.importScale, mapTransform))
	{
		if (renderer3D.isEntity(mapEntity))
		{
			renderer3D.deleteEntity(mapEntity);
		}
		mapEntity = {};
		return false;
	}

	gameplay.loadedMapIndex = resolvedMapIndex;
	gameplay.hasLoadedMap = true;

	if (resetPlayersToSpawn)
	{
		resetClientPositionsToSpawn(gameplay);
		gameplay.transientTopMessage.clear();
		gameplay.transientTopMessageTimer = 0.0f;
	}

	return true;
}

bool paintInterpolatedStrokeScreenSpace(ClientGameplay &gameplay,
	glm::ivec2 fromScreenPosition,
	glm::ivec2 toScreenPosition,
	gl3d::PaintTargetSample &lastSuccessfulSample)
{
	auto &playerPaintColorHsv = gameplay.playerPaintColorHsv;
	auto &playerPaintBrushRadius = gameplay.playerPaintBrushRadius;
	auto &renderer3D = gameplay.renderer3D;

	lastSuccessfulSample = {};
	const glm::vec3 paintColor = hsvToRgb(playerPaintColorHsv);

	const float previewRadius = (std::max)(
		1.0f,
		static_cast<float>(playerPaintBrushRadius) * PLAYER_PAINT_BRUSH_PREVIEW_SCALE);
	const float stepDistance = (std::max)(1.0f, previewRadius);
	const glm::vec2 from = glm::vec2(fromScreenPosition);
	const glm::vec2 to = glm::vec2(toScreenPosition);
	const float distance = glm::distance(from, to);
	const int interpolationSteps = (std::max)(1, static_cast<int>(std::ceil(distance / stepDistance)));
	std::vector<PlayerPaintTexture *> touchedTextures;
	bool paintedAny = false;

	for (int step = 0; step <= interpolationSteps; ++step)
	{
		const float t = static_cast<float>(step) / static_cast<float>(interpolationSteps);
		const glm::ivec2 interpolatedScreenPosition = glm::ivec2(glm::round(glm::mix(from, to, t)));
		gl3d::PaintTargetSample sample = {};
		if (!renderer3D.sampleEntityPaintTarget(interpolatedScreenPosition, sample))
		{
			continue;
		}

		PlayerPaintTexture *paintTexture = getPlayerPaintTexture(gameplay, sample.meshIndex);
		if (paintTexture == nullptr)
		{
			continue;
		}

		paintTextureColor(gameplay, *paintTexture, sample.texturePixel, paintColor);
		if (std::find(touchedTextures.begin(), touchedTextures.end(), paintTexture) == touchedTextures.end())
		{
			touchedTextures.push_back(paintTexture);
		}

		paintedAny = true;
		lastSuccessfulSample = sample;
	}

	for (PlayerPaintTexture *paintTexture : touchedTextures)
	{
		paintTexture->networkDirty = true;
		uploadPlayerPaintTexture(gameplay, *paintTexture);
	}

	if (paintedAny)
	{
		gameplay.localPaintTexturesDirty = true;
	}

	return paintedAny;
}

void setupPlayerPaintTextures(ClientGameplay &gameplay)
{
	setupEntityPaintTextures(gameplay, gameplay.playerEntity, gameplay.playerPaintTextures, true);
}

void updatePaintBrushSize(ClientGameplay &gameplay, const platform::Input &input)
{
	auto &paintDebugState = gameplay.paintDebugState;
	auto &playerPaintBrushRadiusPrecise = gameplay.playerPaintBrushRadiusPrecise;
	auto &playerPaintBrushRadius = gameplay.playerPaintBrushRadius;

	paintDebugState.brushResizeActive = isPaintBrushResizeActive(gameplay, input);

	static glm::vec2 lastPaintBrushResizeMousePosition = {};
	const glm::vec2 resizeDelta = consumeRawMouseDragDelta(
		lastPaintBrushResizeMousePosition,
		paintDebugState.brushResizeActive);

	if (!paintDebugState.brushResizeActive)
	{
		return;
	}

	playerPaintBrushRadiusPrecise = std::clamp(
		playerPaintBrushRadiusPrecise + resizeDelta.x * PLAYER_PAINT_BRUSH_RESIZE_SPEED,
		static_cast<float>(PLAYER_PAINT_BRUSH_RADIUS_MIN),
		static_cast<float>(PLAYER_PAINT_BRUSH_RADIUS_MAX));
	playerPaintBrushRadius = static_cast<int>(std::round(playerPaintBrushRadiusPrecise));
}

void pickPaintColorFromScreen(ClientGameplay &gameplay, platform::Input &input)
{
	auto &paintModeActive = gameplay.paintModeActive;
	auto &cameraMode = gameplay.cameraMode;
	auto &paintColorPickModeActive = gameplay.paintColorPickModeActive;
	auto &paintColorUiCapturingMouse = gameplay.paintColorUiCapturingMouse;
	auto &playerPaintColorHsv = gameplay.playerPaintColorHsv;

	if (!paintModeActive || cameraMode != CameraMode::ThirdPerson)
	{
		paintColorPickModeActive = false;
		return;
	}

	if (!paintColorPickModeActive)
	{
		return;
	}

	if (paintColorUiCapturingMouse || !input.isLMousePressed())
	{
		return;
	}

	glm::vec3 sampledColor = {};
	if (sampleVisibleScreenColor(getMouseFramebufferPosition(gameplay, input), sampledColor))
	{
		playerPaintColorHsv = rgbToHsv(sampledColor);
	}

	paintColorPickModeActive = false;
	resetPaintStrokeState(gameplay);
}

void paintPlayerFromCursor(ClientGameplay &gameplay, platform::Input &input)
{
	auto &paintDebugState = gameplay.paintDebugState;
	auto &paintModeActive = gameplay.paintModeActive;
	auto &cameraMode = gameplay.cameraMode;
	auto &paintColorUiCapturingMouse = gameplay.paintColorUiCapturingMouse;
	auto &paintColorPickModeActive = gameplay.paintColorPickModeActive;
	auto &renderer3D = gameplay.renderer3D;
	auto &hasLastPaintStrokeScreenPosition = gameplay.hasLastPaintStrokeScreenPosition;
	auto &lastPaintStrokeScreenPosition = gameplay.lastPaintStrokeScreenPosition;

	paintDebugState.hoverValid = false;
	paintDebugState.clickValid = false;

	if (!paintModeActive || cameraMode != CameraMode::ThirdPerson)
	{
		resetPaintStrokeState(gameplay);
		return;
	}

	if (paintColorUiCapturingMouse)
	{
		resetPaintStrokeState(gameplay);
		return;
	}

	if (paintColorPickModeActive)
	{
		resetPaintStrokeState(gameplay);
		return;
	}

	const glm::ivec2 currentScreenPosition = getMouseFramebufferPosition(gameplay, input);
	gl3d::PaintTargetSample hoverSample = {};
	if (!renderer3D.sampleEntityPaintTarget(currentScreenPosition, hoverSample))
	{
		paintDebugState.hoverValid = false;
	}
	else
	{
		paintDebugState.hoverValid = true;
		paintDebugState.hoverSample = hoverSample;
	}

	if (paintDebugState.brushResizeActive)
	{
		resetPaintStrokeState(gameplay);
		return;
	}

	if (!input.isLMouseHeld())
	{
		resetPaintStrokeState(gameplay);
		return;
	}

	const glm::ivec2 strokeStartPosition = hasLastPaintStrokeScreenPosition
		? lastPaintStrokeScreenPosition
		: currentScreenPosition;

	gl3d::PaintTargetSample lastSuccessfulSample = {};
	paintDebugState.clickValid = paintInterpolatedStrokeScreenSpace(
		gameplay,
		strokeStartPosition,
		currentScreenPosition,
		lastSuccessfulSample);
	if (paintDebugState.clickValid)
	{
		paintDebugState.clickSample = lastSuccessfulSample;
	}

	hasLastPaintStrokeScreenPosition = true;
	lastPaintStrokeScreenPosition = currentScreenPosition;
}

void renderPaintColorPicker(ClientGameplay &gameplay, gl2d::Renderer2D &renderer, gl2d::Font &paintUiFont)
{
	auto &paintModeActive = gameplay.paintModeActive;
	auto &cameraMode = gameplay.cameraMode;
	auto &playerPaintColorHsv = gameplay.playerPaintColorHsv;
	auto &paintDebugState = gameplay.paintDebugState;
	auto &paintColorPickModeActive = gameplay.paintColorPickModeActive;

	if (!paintModeActive || cameraMode != CameraMode::ThirdPerson)
	{
		return;
	}

	const gl2d::Rect panel = getPaintColorPanelRect();
	const glm::vec3 currentPaintColor = hsvToRgb(playerPaintColorHsv);
	const gl2d::Rect swatchRect = {panel.x + panel.z - 46.0f, panel.y + 12.0f, 28.0f, 28.0f};
	const gl2d::Rect pickButtonRect = getPaintColorPickButtonRect();
	const bool pickButtonHovered = pointInRect(glm::vec2(paintDebugState.framebufferMousePosition), pickButtonRect);

	renderer.renderRectangle(panel, {0.05f, 0.06f, 0.08f, 0.88f});
	renderer.renderRectangleOutline(panel, {0.0f, 0.0f, 0.0f, 0.9f}, 2.0f);
	renderer.renderRectangle(swatchRect, toColor4f(currentPaintColor, 1.0f));
	renderer.renderRectangleOutline(swatchRect, Colors_White, 2.0f);

	for (PaintColorSlider slider : {PaintColorSlider::Hue, PaintColorSlider::Saturation, PaintColorSlider::Value})
	{
		const gl2d::Rect sliderRect = getPaintColorSliderRect(slider);
		const float segmentWidth = sliderRect.z / PAINT_COLOR_SLIDER_SEGMENTS;

		for (int segment = 0; segment < static_cast<int>(PAINT_COLOR_SLIDER_SEGMENTS); ++segment)
		{
			const float t0 = static_cast<float>(segment) / PAINT_COLOR_SLIDER_SEGMENTS;
			const float t1 = static_cast<float>(segment + 1) / PAINT_COLOR_SLIDER_SEGMENTS;
			const float mid = (t0 + t1) * 0.5f;

			renderer.renderRectangle(
				{sliderRect.x + segmentWidth * segment, sliderRect.y, segmentWidth + 1.0f, sliderRect.w},
				toColor4f(getPaintColorSliderPreview(gameplay, slider, mid), 1.0f));
		}

		renderer.renderRectangleOutline(sliderRect, {0.0f, 0.0f, 0.0f, 0.95f}, 2.0f);

		const float *sliderValue = getPaintColorSliderValue(gameplay, slider);
		if (sliderValue != nullptr)
		{
			const float handleCenterX = sliderRect.x + std::clamp(*sliderValue, 0.0f, 1.0f) * sliderRect.z;
			const gl2d::Rect handleOuterRect = {
				handleCenterX - 2.0f,
				sliderRect.y - 4.0f,
				4.0f,
				sliderRect.w + 8.0f
			};
			const gl2d::Rect handleInnerRect = {
				handleCenterX - 1.0f,
				sliderRect.y - 3.0f,
				2.0f,
				sliderRect.w + 6.0f
			};
			renderer.renderRectangle(handleOuterRect, Colors_Black);
			renderer.renderRectangle(handleInnerRect, Colors_White);
		}
	}

	gl2d::Color4f pickButtonColor = {0.12f, 0.14f, 0.18f, 0.96f};
	if (paintColorPickModeActive)
	{
		pickButtonColor = {0.22f, 0.42f, 0.32f, 0.98f};
	}
	else if (pickButtonHovered)
	{
		pickButtonColor = {0.18f, 0.21f, 0.27f, 0.98f};
	}

	renderer.renderRectangle(pickButtonRect, pickButtonColor);
	renderer.renderRectangleOutline(
		pickButtonRect,
		paintColorPickModeActive ? Colors_White : gl2d::Color4f{0.0f, 0.0f, 0.0f, 0.95f},
		2.0f);

	if (paintUiFont.texture.id != 0)
	{
		renderer.renderText({panel.x + 18.0f, panel.y + 22.0f}, "PAINT", paintUiFont, Colors_White, 22.0f, 2.0f, 2.0f, false);
		renderer.renderText({panel.x + 18.0f, getPaintColorSliderRect(PaintColorSlider::Hue).y + 15.0f}, "H", paintUiFont, Colors_White, 18.0f, 2.0f, 2.0f, false);
		renderer.renderText({panel.x + 18.0f, getPaintColorSliderRect(PaintColorSlider::Saturation).y + 15.0f}, "S", paintUiFont, Colors_White, 18.0f, 2.0f, 2.0f, false);
		renderer.renderText({panel.x + 18.0f, getPaintColorSliderRect(PaintColorSlider::Value).y + 15.0f}, "V", paintUiFont, Colors_White, 18.0f, 2.0f, 2.0f, false);
		renderer.renderText(
			{pickButtonRect.x + 12.0f, pickButtonRect.y + 16.0f},
			paintColorPickModeActive ? "PICKING" : "PICK SCREEN (C)",
			paintUiFont,
			Colors_White,
			16.0f,
			2.0f,
			2.0f,
			false);
	}
}

void renderPaintBrushOverlay(ClientGameplay &gameplay, const platform::Input &input, gl2d::Renderer2D &renderer)
{
	auto &paintModeActive = gameplay.paintModeActive;
	auto &cameraMode = gameplay.cameraMode;
	auto &playerPaintColorHsv = gameplay.playerPaintColorHsv;
	auto &paintColorPickModeActive = gameplay.paintColorPickModeActive;
	auto &playerPaintBrushRadius = gameplay.playerPaintBrushRadius;
	auto &paintDebugState = gameplay.paintDebugState;

	if (!paintModeActive || cameraMode != CameraMode::ThirdPerson)
	{
		return;
	}

	const glm::vec2 mousePosition = glm::vec2(getMouseFramebufferPosition(gameplay, input));
	const glm::vec3 currentPaintColor = hsvToRgb(playerPaintColorHsv);

	if (paintColorPickModeActive)
	{
		const gl2d::Rect outerRect = {mousePosition.x - 11.0f, mousePosition.y - 11.0f, 22.0f, 22.0f};
		const gl2d::Rect innerRect = {mousePosition.x - 9.0f, mousePosition.y - 9.0f, 18.0f, 18.0f};
		const gl2d::Rect swatchRect = {mousePosition.x + 16.0f, mousePosition.y + 16.0f, 18.0f, 18.0f};

		renderer.renderRectangleOutline(outerRect, Colors_Black, 3.0f);
		renderer.renderRectangleOutline(innerRect, Colors_White, 1.0f);
		renderer.renderRectangle(swatchRect, toColor4f(currentPaintColor, 1.0f));
		renderer.renderRectangleOutline(swatchRect, Colors_Black, 2.0f);
		return;
	}

	const float previewRadius = (std::max)(
		8.0f,
		static_cast<float>(playerPaintBrushRadius) * PLAYER_PAINT_BRUSH_PREVIEW_SCALE);

	gl2d::Color4f brushColor = paintDebugState.hoverValid
		? gl2d::Color4f{1.0f, 1.0f, 1.0f, 0.95f}
		: gl2d::Color4f{1.0f, 0.35f, 0.35f, 0.95f};

	if (paintDebugState.brushResizeActive)
	{
		brushColor = {1.0f, 0.85f, 0.2f, 0.98f};
	}

	renderer.renderCircleOutline(mousePosition, previewRadius + 1.0f, {0.0f, 0.0f, 0.0f, 0.9f}, 3.0f, 40);
	renderer.renderCircleOutline(mousePosition, previewRadius, brushColor, 2.0f, 40);
}

void updateThirdPersonCameraZoom(ClientGameplay &gameplay, bool ignoreImguiCapture = false)
{
	auto &thirdPersonCameraDistance = gameplay.thirdPersonCameraDistance;

	ImGuiIO &io = ImGui::GetIO();
	if ((!ignoreImguiCapture && io.WantCaptureMouse) || std::abs(io.MouseWheel) < 0.001f)
	{
		return;
	}

	thirdPersonCameraDistance = std::clamp(
		thirdPersonCameraDistance - io.MouseWheel * THIRD_PERSON_CAMERA_ZOOM_STEP,
		THIRD_PERSON_CAMERA_DISTANCE_MIN,
		THIRD_PERSON_CAMERA_DISTANCE_MAX);
}

void updateThirdPersonCamera(ClientGameplay &gameplay)
{
	auto &playerPhysics = gameplay.playerPhysics;
	auto &thirdPersonYaw = gameplay.thirdPersonYaw;
	auto &thirdPersonPitch = gameplay.thirdPersonPitch;
	auto &thirdPersonCameraDistance = gameplay.thirdPersonCameraDistance;
	auto &renderer3D = gameplay.renderer3D;

	const glm::vec3 playerPosition = playerPhysics.getPlayerPosition();
	const glm::vec3 cameraForward = buildThirdPersonForward(thirdPersonYaw, thirdPersonPitch);
	const glm::vec3 target = playerPosition + glm::vec3(0.0f, getThirdPersonTargetHeight(gameplay), 0.0f);

	renderer3D.camera.viewDirection = cameraForward;
	renderer3D.camera.position = target - cameraForward * thirdPersonCameraDistance;
}

void updateFirstPersonCamera(ClientGameplay &gameplay)
{
	auto &playerPhysics = gameplay.playerPhysics;
	auto &thirdPersonYaw = gameplay.thirdPersonYaw;
	auto &thirdPersonPitch = gameplay.thirdPersonPitch;
	auto &renderer3D = gameplay.renderer3D;

	const glm::vec3 eyePosition = playerPhysics.getPlayerPosition()
		+ glm::vec3(0.0f, getThirdPersonTargetHeight(gameplay), 0.0f);
	renderer3D.camera.position = eyePosition;
	renderer3D.camera.viewDirection = buildThirdPersonForward(thirdPersonYaw, thirdPersonPitch);
}

void updateLocalPlayerVisibility(ClientGameplay &gameplay)
{
	auto &renderer3D = gameplay.renderer3D;
	auto &playerEntity = gameplay.playerEntity;
	const bool shouldBeVisible = !isHunterFirstPersonActive(gameplay);

	if (renderer3D.isEntity(playerEntity) && renderer3D.isEntityVisible(playerEntity) != shouldBeVisible)
	{
		renderer3D.setEntityVisible(playerEntity, shouldBeVisible);
	}
}

void showTransientTopMessage(ClientGameplay &gameplay, std::string message, float duration)
{
	gameplay.transientTopMessage = std::move(message);
	gameplay.transientTopMessageTimer = duration;
}

bool intersectRaySphere(const glm::vec3 &origin, const glm::vec3 &direction,
	const glm::vec3 &center, float radius, float &hitDistance)
{
	const glm::vec3 offset = origin - center;
	const float b = glm::dot(offset, direction);
	const float c = glm::dot(offset, offset) - radius * radius;
	const float discriminant = b * b - c;
	if (discriminant < 0.0f)
	{
		return false;
	}

	const float sqrtDiscriminant = std::sqrt(discriminant);
	float resolvedDistance = -b - sqrtDiscriminant;
	if (resolvedDistance < 0.0f)
	{
		resolvedDistance = -b + sqrtDiscriminant;
	}

	if (resolvedDistance < 0.0f)
	{
		return false;
	}

	hitDistance = resolvedDistance;
	return true;
}

bool intersectRemotePlayerHitCollider(const ClientGameplay &gameplay, std::uint64_t cid,
	const RemotePlayerVisual &remotePlayer, const glm::vec3 &rayOrigin, const glm::vec3 &rayDirection,
	float maxDistance, float &hitDistance)
{
	if (!remotePlayer.hasVisualState)
	{
		return false;
	}

	const float scale = getPlayerVisualScale(gameplay, cid);
	const float radius = PLAYER_RAYCAST_SPHERE_RADIUS * scale;
	const glm::vec3 basePosition = remotePlayer.visualPosition;
	const glm::vec3 sphereCenters[] =
	{
		basePosition + glm::vec3(0.0f, 0.55f * scale, 0.0f),
		basePosition + glm::vec3(0.0f, 1.15f * scale, 0.0f),
		basePosition + glm::vec3(0.0f, 1.75f * scale, 0.0f),
	};

	bool foundIntersection = false;
	hitDistance = maxDistance;

	for (const glm::vec3 &sphereCenter : sphereCenters)
	{
		float sphereHitDistance = 0.0f;
		if (intersectRaySphere(rayOrigin, rayDirection, sphereCenter, radius, sphereHitDistance)
			&& sphereHitDistance <= maxDistance
			&& sphereHitDistance < hitDistance)
		{
			hitDistance = sphereHitDistance;
			foundIntersection = true;
		}
	}

	return foundIntersection;
}

bool tryFindHunterShotTarget(ClientGameplay &gameplay, std::uint64_t &targetCID)
{
	auto &clientNetworking = gameplay.clientNetworking;
	auto &renderer3D = gameplay.renderer3D;
	auto &playerPhysics = gameplay.playerPhysics;
	auto &remotePlayers = gameplay.remotePlayers;

	targetCID = 0;

	glm::vec3 rayDirection = renderer3D.camera.viewDirection;
	const float directionLength = glm::length(rayDirection);
	if (directionLength <= 0.0001f)
	{
		return false;
	}

	rayDirection /= directionLength;
	const glm::vec3 rayOrigin = renderer3D.camera.position;

	float blockingGeometryDistance = HUNTER_SHOT_MAX_DISTANCE;
	const bool hitBlockingGeometry = playerPhysics.raycastStaticGeometry(
		rayOrigin,
		rayDirection,
		HUNTER_SHOT_MAX_DISTANCE,
		blockingGeometryDistance);

	bool foundTarget = false;
	float bestHitDistance = hitBlockingGeometry ? blockingGeometryDistance : HUNTER_SHOT_MAX_DISTANCE;

	for (auto &[cid, remotePlayer] : remotePlayers)
	{
		if (cid == 0
			|| cid == clientNetworking.hunterCID
			|| clientNetworking.isPlayerFound(cid))
		{
			continue;
		}

		float playerHitDistance = HUNTER_SHOT_MAX_DISTANCE;
		if (intersectRemotePlayerHitCollider(
			gameplay,
			cid,
			remotePlayer,
			rayOrigin,
			rayDirection,
			HUNTER_SHOT_MAX_DISTANCE,
			playerHitDistance)
			&& playerHitDistance <= bestHitDistance)
		{
			bestHitDistance = playerHitDistance;
			targetCID = cid;
			foundTarget = true;
		}
	}

	return foundTarget;
}

void updateTransientGameplayMessage(ClientGameplay &gameplay, float deltaTime)
{
	auto &transientTopMessageTimer = gameplay.transientTopMessageTimer;
	auto &transientTopMessage = gameplay.transientTopMessage;
	auto &lastSeenRoundResult = gameplay.lastSeenRoundResult;
	auto &clientNetworking = gameplay.clientNetworking;

	if (transientTopMessageTimer > 0.0f)
	{
		transientTopMessageTimer = (std::max)(0.0f, transientTopMessageTimer - deltaTime);
		if (transientTopMessageTimer <= 0.0f)
		{
			transientTopMessage.clear();
		}
	}

	if (clientNetworking.roundResult != lastSeenRoundResult)
	{
		lastSeenRoundResult = clientNetworking.roundResult;
		if (lastSeenRoundResult == ClientNetworking::RoundResult::HunterWon)
		{
			showTransientTopMessage(gameplay, "Hunter won.", ROUND_RESULT_MESSAGE_DURATION);
		}
		else if (clientNetworking.gameActive)
		{
			transientTopMessage.clear();
			transientTopMessageTimer = 0.0f;
		}
	}
}

void handleHunterShotInput(ClientGameplay &gameplay, const platform::Input &input)
{
	auto &clientNetworking = gameplay.clientNetworking;

	if (!isHunterFirstPersonActive(gameplay)
		|| gameplay.paintModeActive
		|| !input.lMouse.pressed
		|| ImGui::GetIO().WantCaptureMouse)
	{
		return;
	}

	std::uint64_t targetCID = 0;
	if (tryFindHunterShotTarget(gameplay, targetCID)
		&& clientNetworking.sendHunterHitPlayer(targetCID))
	{
		showTransientTopMessage(gameplay, HUNTER_FOUND_MESSAGE_TEXT, HUNTER_FOUND_MESSAGE_DURATION);
	}
}

void renderTopStatusMessage(gl2d::Renderer2D &renderer, gl2d::Font &font, const std::string &message,
	float topOffset, gl2d::Color4f panelColor, gl2d::Color4f textColor)
{
	if (message.empty() || font.texture.id == 0 || renderer.windowW <= 0 || renderer.windowH <= 0)
	{
		return;
	}

	constexpr float cTextSize = 18.0f;
	constexpr float cPaddingX = 14.0f;
	constexpr float cPaddingY = 10.0f;

	const glm::vec2 textSize = renderer.getTextSize(message.c_str(), font, cTextSize, 2.0f, 2.0f);
	const gl2d::Rect panel = {
		(renderer.windowW - (textSize.x + cPaddingX * 2.0f)) * 0.5f,
		topOffset,
		textSize.x + cPaddingX * 2.0f,
		textSize.y + cPaddingY * 2.0f
	};

	renderer.renderRectangle(panel, panelColor);
	renderer.renderRectangleOutline(panel, {0.0f, 0.0f, 0.0f, 0.92f}, 2.0f);
	renderer.renderText(
		{panel.x + cPaddingX, panel.y + cPaddingY + textSize.y},
		message.c_str(),
		font,
		textColor,
		cTextSize,
		2.0f,
		2.0f,
		false);
}

void renderLargeTopStatusMessage(gl2d::Renderer2D &renderer, gl2d::Font &font, const std::string &message,
	float topOffset, gl2d::Color4f panelColor, gl2d::Color4f textColor)
{
	if (message.empty() || font.texture.id == 0 || renderer.windowW <= 0 || renderer.windowH <= 0)
	{
		return;
	}

	constexpr float cTextSize = 34.0f;
	constexpr float cPaddingX = 24.0f;
	constexpr float cPaddingY = 14.0f;

	const glm::vec2 textSize = renderer.getTextSize(message.c_str(), font, cTextSize, 2.0f, 2.0f);
	const gl2d::Rect panel = {
		(renderer.windowW - (textSize.x + cPaddingX * 2.0f)) * 0.5f,
		topOffset,
		textSize.x + cPaddingX * 2.0f,
		textSize.y + cPaddingY * 2.0f
	};

	renderer.renderRectangle(panel, panelColor);
	renderer.renderRectangleOutline(panel, {0.0f, 0.0f, 0.0f, 0.95f}, 3.0f);
	renderer.renderText(
		{panel.x + cPaddingX, panel.y + cPaddingY + textSize.y},
		message.c_str(),
		font,
		textColor,
		cTextSize,
		2.0f,
		2.0f,
		false);
}

void renderHunterCrosshair(const ClientGameplay &gameplay, gl2d::Renderer2D &renderer)
{
	if (!isHunterFirstPersonActive(gameplay) || renderer.windowW <= 0 || renderer.windowH <= 0)
	{
		return;
	}

	const float centerX = renderer.windowW * 0.5f;
	const float centerY = renderer.windowH * 0.5f;
	constexpr float cGap = 3.0f;
	constexpr float cArmLength = 8.0f;
	constexpr float cOuterThickness = 3.0f;
	constexpr float cInnerThickness = 1.0f;

	const gl2d::Color4f outerColor = {0.0f, 0.0f, 0.0f, 0.95f};
	const gl2d::Color4f innerColor = {0.96f, 0.96f, 0.96f, 0.95f};

	const gl2d::Rect horizontalOuter = {
		centerX - cGap - cArmLength,
		centerY - cOuterThickness * 0.5f,
		cArmLength * 2.0f + cGap * 2.0f,
		cOuterThickness
	};
	const gl2d::Rect verticalOuter = {
		centerX - cOuterThickness * 0.5f,
		centerY - cGap - cArmLength,
		cOuterThickness,
		cArmLength * 2.0f + cGap * 2.0f
	};

	const gl2d::Rect horizontalInnerLeft = {
		centerX - cGap - cArmLength,
		centerY - cInnerThickness * 0.5f,
		cArmLength,
		cInnerThickness
	};
	const gl2d::Rect horizontalInnerRight = {
		centerX + cGap,
		centerY - cInnerThickness * 0.5f,
		cArmLength,
		cInnerThickness
	};
	const gl2d::Rect verticalInnerTop = {
		centerX - cInnerThickness * 0.5f,
		centerY - cGap - cArmLength,
		cInnerThickness,
		cArmLength
	};
	const gl2d::Rect verticalInnerBottom = {
		centerX - cInnerThickness * 0.5f,
		centerY + cGap,
		cInnerThickness,
		cArmLength
	};

	renderer.renderRectangle(horizontalOuter, outerColor);
	renderer.renderRectangle(verticalOuter, outerColor);
	renderer.renderRectangle(horizontalInnerLeft, innerColor);
	renderer.renderRectangle(horizontalInnerRight, innerColor);
	renderer.renderRectangle(verticalInnerTop, innerColor);
	renderer.renderRectangle(verticalInnerBottom, innerColor);
}

void renderHunterCameleonTracker(const ClientGameplay &gameplay, gl2d::Renderer2D &renderer, gl2d::Font &font)
{
	auto &clientNetworking = gameplay.clientNetworking;
	if (!clientNetworking.gameActive
		|| !clientNetworking.isLocalHunter()
		|| font.texture.id == 0
		|| renderer.windowW <= 0
		|| renderer.windowH <= 0)
	{
		return;
	}

	const int totalCameleons = (std::max)(0, static_cast<int>(clientNetworking.totalHiderCount));
	const int foundCameleons = (std::min)(
		totalCameleons,
		static_cast<int>(clientNetworking.foundHiderIDs.size()));

	constexpr char cTitle[] = "Cameleons";
	constexpr float cPanelMargin = 18.0f;
	constexpr float cPanelPadding = 14.0f;
	constexpr float cTitleSize = 18.0f;
	constexpr float cTitleGap = 12.0f;
	constexpr float cCircleRadius = 10.0f;
	constexpr float cCircleSpacing = 10.0f;
	constexpr float cRowSpacing = 12.0f;
	constexpr int cMaxCirclesPerRow = 8;

	const glm::vec2 titleTextSize = renderer.getTextSize(cTitle, font, cTitleSize, 2.0f, 2.0f);
	const int rowCount = totalCameleons > 0 ? ((totalCameleons + cMaxCirclesPerRow - 1) / cMaxCirclesPerRow) : 0;
	const int widestRowCircleCount = totalCameleons > 0 ? (std::min)(totalCameleons, cMaxCirclesPerRow) : 0;
	const float circleDiameter = cCircleRadius * 2.0f;
	const float circlesWidth = widestRowCircleCount > 0
		? widestRowCircleCount * circleDiameter + (widestRowCircleCount - 1) * cCircleSpacing
		: 0.0f;
	const float circlesHeight = rowCount > 0
		? rowCount * circleDiameter + (rowCount - 1) * cRowSpacing
		: 0.0f;

	const float panelWidth = (std::max)(titleTextSize.x, circlesWidth) + cPanelPadding * 2.0f;
	const float panelHeight = cPanelPadding * 2.0f
		+ titleTextSize.y
		+ (rowCount > 0 ? (cTitleGap + circlesHeight) : 0.0f);
	const gl2d::Rect panel = {
		renderer.windowW - panelWidth - cPanelMargin,
		cPanelMargin,
		panelWidth,
		panelHeight
	};

	renderer.renderRectangle(panel, {0.06f, 0.08f, 0.11f, 0.78f});
	renderer.renderRectangleOutline(panel, {0.0f, 0.0f, 0.0f, 0.92f}, 2.0f);
	renderer.renderText(
		{panel.x + cPanelPadding, panel.y + cPanelPadding + titleTextSize.y},
		cTitle,
		font,
		Colors_White,
		cTitleSize,
		2.0f,
		2.0f,
		false);

	for (int circleIndex = 0; circleIndex < totalCameleons; ++circleIndex)
	{
		const int rowIndex = circleIndex / cMaxCirclesPerRow;
		const int columnIndex = circleIndex % cMaxCirclesPerRow;
		const int circlesInThisRow = (std::min)(cMaxCirclesPerRow, totalCameleons - rowIndex * cMaxCirclesPerRow);
		const float rowWidth = circlesInThisRow * circleDiameter + (circlesInThisRow - 1) * cCircleSpacing;
		const float rowStartX = panel.x + panel.w - cPanelPadding - rowWidth;
		const glm::vec2 center = {
			rowStartX + cCircleRadius + columnIndex * (circleDiameter + cCircleSpacing),
			panel.y + cPanelPadding + titleTextSize.y + cTitleGap + cCircleRadius
				+ rowIndex * (circleDiameter + cRowSpacing)
		};

		renderer.renderCircleOutline(center, cCircleRadius + 1.0f, {0.0f, 0.0f, 0.0f, 0.95f}, 4.0f, 28);
		renderer.renderCircleOutline(center, cCircleRadius, Colors_White, 2.0f, 28);

		if (circleIndex < foundCameleons)
		{
			const glm::vec2 offset = {cCircleRadius * 0.72f, cCircleRadius * 0.72f};
			const glm::vec2 topLeft = center - offset;
			const glm::vec2 topRight = {center.x + offset.x, center.y - offset.y};
			const glm::vec2 bottomLeft = {center.x - offset.x, center.y + offset.y};
			const glm::vec2 bottomRight = center + offset;

			renderer.renderLine(topLeft, bottomRight, {0.0f, 0.0f, 0.0f, 0.92f}, 5.0f);
			renderer.renderLine(topRight, bottomLeft, {0.0f, 0.0f, 0.0f, 0.92f}, 5.0f);
			renderer.renderLine(topLeft, bottomRight, {0.86f, 0.12f, 0.12f, 0.98f}, 2.5f);
			renderer.renderLine(topRight, bottomLeft, {0.86f, 0.12f, 0.12f, 0.98f}, 2.5f);
		}
	}
}

void renderGameplayNotifications(ClientGameplay &gameplay, gl2d::Renderer2D &renderer, gl2d::Font &font)
{
	auto &clientNetworking = gameplay.clientNetworking;
	auto &transientTopMessage = gameplay.transientTopMessage;
	auto &transientTopMessageTimer = gameplay.transientTopMessageTimer;

	renderer.pushCamera();
	renderHunterCrosshair(gameplay, renderer);
	renderHunterCameleonTracker(gameplay, renderer, font);

	float topOffset = 18.0f;
	if (clientNetworking.localPlayerFound)
	{
		renderTopStatusMessage(
			renderer,
			font,
			"You were hit.",
			topOffset,
			{0.42f, 0.11f, 0.11f, 0.9f},
			Colors_White);
		topOffset += 44.0f;
	}

	if (transientTopMessageTimer > 0.0f && !transientTopMessage.empty())
	{
		const bool roundWonMessage = clientNetworking.roundResult == ClientNetworking::RoundResult::HunterWon
			&& transientTopMessage == "Hunter won.";
		const bool foundCameleonMessage = transientTopMessage == HUNTER_FOUND_MESSAGE_TEXT;
		if (foundCameleonMessage)
		{
			renderLargeTopStatusMessage(
				renderer,
				font,
				transientTopMessage,
				topOffset,
				{0.10f, 0.38f, 0.12f, 0.94f},
				Colors_White);
			topOffset += 64.0f;
		}
		else
		{
			renderTopStatusMessage(
				renderer,
				font,
				transientTopMessage,
				topOffset,
				roundWonMessage
					? gl2d::Color4f{0.45f, 0.32f, 0.08f, 0.9f}
					: gl2d::Color4f{0.12f, 0.32f, 0.18f, 0.9f},
				Colors_White);
			topOffset += 44.0f;
		}
	}

	if (clientNetworking.gameActive
		&& clientNetworking.roundPhase != ClientNetworking::RoundPhase::Lobby)
	{
		const int timerSeconds = (std::max)(
			0,
			static_cast<int>(std::ceil(clientNetworking.roundTimerRemainingSeconds)));
		const std::string timerMessage = std::string(
			clientNetworking.roundPhase == ClientNetworking::RoundPhase::HiderHide
				? "Hide time: "
				: "Hunter time: ")
			+ std::to_string(timerSeconds) + "s";

		renderTopStatusMessage(
			renderer,
			font,
			timerMessage,
			topOffset,
			{0.11f, 0.20f, 0.44f, 0.9f},
			Colors_White);
	}

	renderer.popCamera();
}

bool ClientGameplay::init(const char *serverAddress)
{
	cameraMode = CameraMode::ThirdPerson;
	thirdPersonYaw = 0.0f;
	thirdPersonPitch = glm::radians(-18.0f);
	thirdPersonCameraDistance = THIRD_PERSON_CAMERA_DISTANCE_DEFAULT;
	paintModeActive = false;
	playerPaintTextures.clear();
	playerPaintBrushRadius = PLAYER_PAINT_BRUSH_RADIUS_DEFAULT;
	playerPaintBrushRadiusPrecise = static_cast<float>(PLAYER_PAINT_BRUSH_RADIUS_DEFAULT);
	playerPaintColorHsv = {0.0f, 1.0f, 0.0f};
	paintColorPickModeActive = false;
	paintColorUiHovered = false;
	paintColorUiCapturingMouse = false;
	activePaintColorSlider = PaintColorSlider::None;
	paintDebugState = {};
	hasLastPaintStrokeScreenPosition = false;
	lastPaintStrokeScreenPosition = {};
	localAnimationIndex = 9;
	timeSinceLastUnreliablePlayerStateSent = 0.0f;
	timeSinceLastReliablePlayerStateSent = 0.0f;
	hasLastSentPlayerState = false;
	lastSentPlayerState = {};
	clientNetworking.remotePlayers.clear();
	clientNetworking.remotePaintUpdates.clear();
	timeSinceLastPaintTextureSync = 0.0f;
	localPaintTexturesDirty = false;
	hunterHidePhaseForcedPaintMode = false;
	transientTopMessage.clear();
	transientTopMessageTimer = 0.0f;
	lastSeenRoundResult = ClientNetworking::RoundResult::None;
	lastSeenRoundPhase = ClientNetworking::RoundPhase::Lobby;
	loadedMapIndex = 0;
	hasLoadedMap = false;
	loadedMapModels.clear();
	clearRemotePlayers(*this);

	if (!clientNetworking.connectToServer(serverAddress))
	{
		return false;
	}

#pragma region init stuff
	platform::showMouse(false);

	renderer3D.init(1, 1, RESOURCES_PATH "BRDFintegrationMap.png");
	renderer3D.frustumCulling = false;

	//renderer3D.skyBox = renderer3D.loadHDRSkyBox(RESOURCES_PATH "sky.hdr");
	renderer3D.skyBox = renderer3D.loadSkyBox(RESOURCES_PATH "skybox.png");
	renderer3D.skyBox.color *= 0.9f;

	playerModel = renderer3D.loadModel(RESOURCES_PATH "player3.glb", gl3d::maxQuality, 1);

	playerEntity = renderer3D.createEntity(playerModel, {}, false, true, false);
	renderer3D.setEntityAnimate(playerEntity, true);
	setupPlayerPaintTextures(*this);

	if (!playerPhysics.init())
	{
		clientNetworking.shutdown();
		return false;
	}

	if (!loadMapByIndex(*this, clientNetworking.currentMapIndex, true))
	{
		clientNetworking.shutdown();
		playerPhysics.shutdown();
		return false;
	}

	syncPlayerEntityToPhysics(*this);

	//auto grid = platform::createGridModel(
	//	renderer3D,
	//	200.f,
	//	41,
	//	{0.35f, 0.35f, 0.38f, 1.f},
	//	0.f,
	//	0.f,
	//	"worldGrid");
	//renderer3D.createEntity(grid.model, {}, true, true, false);

	renderer3D.createDirectionalLight({0, -1, 0.3f}, glm::vec3(1, 1, 1), 1, false);
	renderer3D.createDirectionalLight({0, -1, -0.3f}, glm::vec3(1, 1, 1), 1, false);
	renderer3D.createDirectionalLight({0.3f, -1, 0.0f}, glm::vec3(0.4f, 0.4f, 0.4f), 1, false);
	renderer3D.createDirectionalLight({-0.3f, 1, 0.0f}, glm::vec3(0.4f, 0.4f, 0.4f), 1, false);

	renderer3D.camera.position = {0.0f, 2.2f, 6.0f};
	renderer3D.camera.viewDirection = glm::normalize(
		glm::vec3(0.0f, getThirdPersonTargetHeight(*this), 0.0f) - renderer3D.camera.position);
	syncThirdPersonOrbitToCamera(*this);
#pragma endregion

	return true;
}

bool ClientGameplay::update(float deltaTime, platform::Input &input, gl2d::Renderer2D &renderer, gl2d::Font &paintUiFont)
{

#pragma region init stuff
	const int w = platform::getFrameBufferSizeX();
	const int h = platform::getFrameBufferSizeY();
#pragma endregion

	clientNetworking.update(deltaTime);
	updateTransientGameplayMessage(*this, deltaTime);
	renderer3D.updateWindowMetrics(w, h);
	playerPhysics.setHunterMode(clientNetworking.isLocalHunter());
	if (!hasLoadedMap || loadedMapIndex != clampGameMapIndex(clientNetworking.currentMapIndex))
	{
		if (!loadMapByIndex(*this, clientNetworking.currentMapIndex, true))
		{
			clientNetworking.lastStatus = "Failed to load the map selected by the server.";
			return false;
		}

		syncPlayerEntityToPhysics(*this);
	}
	const bool enteredRoundStartPhase =
		clientNetworking.roundPhase == ClientNetworking::RoundPhase::HiderHide
		&& lastSeenRoundPhase != ClientNetworking::RoundPhase::HiderHide;
	if (enteredRoundStartPhase)
	{
		resetClientRoundStartState(*this);
	}
	lastSeenRoundPhase = clientNetworking.roundPhase;
	if (isHunterGameplayCameraLocked(*this))
	{
		setCameraMode(*this, CameraMode::ThirdPerson);
	}
	updateForcedHunterHidePaintModeState(*this);

	if (platform::isButtonPressed(platform::Button::Tab)
		&& !isHunterGameplayCameraLocked(*this))
	{
		toggleCameraMode(*this);
	}

	if (cameraMode == CameraMode::ThirdPerson
		&& !hunterHidePhaseForcedPaintMode
		&& platform::isButtonPressed(platform::Button::F))
	{
		paintModeActive = !paintModeActive;
		if (!paintModeActive)
		{
			paintColorPickModeActive = false;
		}
	}

	updatePaintColorPicker(*this, input);
	updatePaintBrushSize(*this, input);
	updateLocalAnimationSelection(*this);

	const bool captureMouseLook = cameraMode == CameraMode::Free || !paintModeActive;
	static glm::vec2 lastMousePosition = {};
	static glm::vec2 lastPaintOrbitMousePosition = {};
	const glm::vec2 lookDelta = consumeLookDelta(lastMousePosition, captureMouseLook);
	const glm::vec2 paintOrbitDelta = consumeMouseDragDelta(
		lastPaintOrbitMousePosition,
		cameraMode == CameraMode::ThirdPerson && paintModeActive && input.isMMouseHeld());

	PhysicsControllerInput playerInput = {};
	if (cameraMode == CameraMode::Free)
	{
		applyFreeCameraInput(renderer3D, 20.0f, deltaTime, lookDelta);
	}
	else
	{
		if (paintModeActive)
		{
			thirdPersonYaw -= paintOrbitDelta.x;
			thirdPersonPitch = std::clamp(
				thirdPersonPitch + paintOrbitDelta.y,
				THIRD_PERSON_PITCH_MIN,
				THIRD_PERSON_PITCH_MAX);
			updateThirdPersonCameraZoom(*this, true);
		}
		else
		{
			thirdPersonYaw -= lookDelta.x;
			thirdPersonPitch = std::clamp(
				thirdPersonPitch + lookDelta.y,
				THIRD_PERSON_PITCH_MIN,
				THIRD_PERSON_PITCH_MAX);
			if (!isHunterFirstPersonActive(*this))
			{
				updateThirdPersonCameraZoom(*this);
			}
			if (!isHunterHidePhaseActive(*this))
			{
				playerInput = buildPlayerInput(*this);
			}
		}
	}

	playerPhysics.update(deltaTime, playerInput);
	syncPlayerEntityToPhysics(*this);
	updateLocalPlayerVisibility(*this);
	const int currentAnimationIndex = getCurrentLocalAnimationIndex(*this, playerInput);
	renderer3D.setEntityAnimationIndex(playerEntity, currentAnimationIndex);
	sendLocalPlayerState(*this, deltaTime, currentAnimationIndex);
	syncRemotePlayers(*this, deltaTime);

	if (cameraMode == CameraMode::ThirdPerson)
	{
		if (isHunterFirstPersonActive(*this))
		{
			updateFirstPersonCamera(*this);
		}
		else
		{
			updateThirdPersonCamera(*this);
		}
	}

	handleHunterShotInput(*this, input);

	renderer3D.render(deltaTime);
	pickPaintColorFromScreen(*this, input);
	paintPlayerFromCursor(*this, input);
	sendLocalPaintTextures(*this, deltaTime);
	renderPaintBrushOverlay(*this, input, renderer);
	renderPaintColorPicker(*this, renderer, paintUiFont);
	renderGameplayNotifications(*this, renderer, paintUiFont);

	if (0)
	{
		ImGuiViewport *mainVp = ImGui::GetMainViewport();

		ImGuiWindowClass windowClass;
		windowClass.DockingAllowUnclassed = false;
		windowClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoDocking;
		windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
		ImGui::SetNextWindowClass(&windowClass);

		ImGui::SetNextWindowPos(
			ImVec2(mainVp->Pos.x + mainVp->Size.x + 20, mainVp->Pos.y + 50),
			ImGuiCond_FirstUseEver
		);

		ImGui::PushMakeWindowNotTransparent();
		ImGui::Begin("Tweaks");

		ImGui::SliderInt("Animation", &localAnimationIndex, 0, PLAYER_ANIMATIONS - 1);
		ImGui::Text("Anim cycle: Q previous / E next");
		const bool hunterCameraLocked = isHunterGameplayCameraLocked(*this);
		const bool hunterFirstPerson = isHunterFirstPersonActive(*this);
		const char *cameraName = cameraMode == CameraMode::Free
			? "Free"
			: (hunterFirstPerson ? "First-Person" : (paintModeActive && hunterCameraLocked ? "Third-Person (Paint)" : "Third-Person"));
		ImGui::Text("Camera: %s", cameraName);
		if (ImGui::Button(
			hunterCameraLocked
				? "Gameplay Camera Locked (Hunter)"
				: (cameraMode == CameraMode::Free ? "Switch to Third-Person (Tab)" : "Switch to Free Camera (Tab)")))
		{
			toggleCameraMode(*this);
		}
		if (hunterCameraLocked)
		{
			ImGui::Text("Hunter hide phase forces paint mode. Hunter search uses first-person.");
		}
		ImGui::Text("Free camera: WASD + Q/E");
		ImGui::Text("Player: WASD move, Shift run / wall descend, Space jump / wall climb");
		ImGui::Text("Hunter: Left click to shoot hiders in first-person");
		ImGui::Text("Paint mode: %s", paintModeActive ? "On" : "Off");
		ImGui::Text("Paint: F toggle, C pick color, Left click paint, Right drag resize, Middle drag orbit, Scroll zoom");
		ImGui::Text("Paint brush radius: %d%s", playerPaintBrushRadius, paintDebugState.brushResizeActive ? " (resizing)" : "");
		ImGui::Text("Color picker tool: %s", paintColorPickModeActive ? "On" : "Off");
		ImGui::Text("Paint textures: %d", static_cast<int>(playerPaintTextures.size()));
		ImGui::Text(
			"Mouse window %d,%d framebuffer %d,%d",
			paintDebugState.windowMousePosition.x,
			paintDebugState.windowMousePosition.y,
			paintDebugState.framebufferMousePosition.x,
			paintDebugState.framebufferMousePosition.y);
		ImGui::Text("ImGui wants mouse: %s", ImGui::GetIO().WantCaptureMouse ? "Yes" : "No");
		ImGui::Text("Paint hover hit: %s", paintDebugState.hoverValid ? "Yes" : "No");
		if (paintDebugState.hoverValid)
		{
			ImGui::Text(
				"Hover mesh %d texel %d,%d / %d,%d",
				paintDebugState.hoverSample.meshIndex,
				paintDebugState.hoverSample.texturePixel.x,
				paintDebugState.hoverSample.texturePixel.y,
				paintDebugState.hoverSample.textureSize.x,
				paintDebugState.hoverSample.textureSize.y);
		}
		ImGui::Text("Paint click hit: %s", paintDebugState.clickValid ? "Yes" : "No");
		if (paintDebugState.clickValid)
		{
			ImGui::Text(
				"Last click mesh %d texel %d,%d",
				paintDebugState.clickSample.meshIndex,
				paintDebugState.clickSample.texturePixel.x,
				paintDebugState.clickSample.texturePixel.y);
		}
		ImGui::Text("Third-person zoom: Mouse wheel (%.1f)", thirdPersonCameraDistance);
		ImGui::Text("Grounded: %s", playerPhysics.isGrounded() ? "Yes" : "No");
		ImGui::Text("Wall attached: %s", playerPhysics.isWallAttached() ? "Yes" : "No");
		ImGui::Separator();
		ImGui::Text("Net state: %s", clientNetworking.getConnectionStateName());
		ImGui::Text("Server IP: %s", clientNetworking.connectedServerAddress.c_str());
		ImGui::Text("Net status: %s", clientNetworking.lastStatus.c_str());
		ImGui::Text("Game active: %s", clientNetworking.gameActive ? "Yes" : "No");
		ImGui::Text(
			"Round phase: %s",
			clientNetworking.roundPhase == ClientNetworking::RoundPhase::HiderHide
				? "Hider Hide"
				: (clientNetworking.roundPhase == ClientNetworking::RoundPhase::HunterSearch ? "Hunter Search" : "Lobby"));
		ImGui::Text("Round timer: %.1fs", clientNetworking.roundTimerRemainingSeconds);
		ImGui::Text("Role: %s", clientNetworking.isLocalHunter() ? "Hunter" : (clientNetworking.gameActive ? "Hider" : "None"));
		ImGui::Text("Hunter CID: %llu", static_cast<unsigned long long>(clientNetworking.hunterCID));
		ImGui::Text("Received player data: %s", clientNetworking.receivedPlayerData ? "Yes" : "No");
		ImGui::Text("Local CID: %llu", static_cast<unsigned long long>(clientNetworking.localCID));
		ImGui::Text("Remote players: %d", static_cast<int>(remotePlayers.size()));

		ImGui::PopMakeWindowNotTransparent();
		ImGui::End();
	}

	return true;
}

void ClientGameplay::shutdown()
{
	clearPaintTextures(*this, playerPaintTextures);
	clearRemotePlayers(*this);
	if (renderer3D.isEntity(mapEntity))
	{
		renderer3D.deleteEntity(mapEntity);
	}
	mapEntity = {};
	mapModel = {};
	loadedMapModels.clear();
	hunterHidePhaseForcedPaintMode = false;
	transientTopMessage.clear();
	transientTopMessageTimer = 0.0f;
	lastSeenRoundResult = ClientNetworking::RoundResult::None;
	lastSeenRoundPhase = ClientNetworking::RoundPhase::Lobby;
	hasLoadedMap = false;
	loadedMapIndex = 0;
	clientNetworking.shutdown();
	playerPhysics.shutdown();
}
