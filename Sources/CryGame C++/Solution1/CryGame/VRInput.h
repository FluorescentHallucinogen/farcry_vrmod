#pragma once
#include "VROpenXR.h"

#undef GetUserName

class CXGame;

// Turns the OpenXR controller state into the game's actions.
//
// SteamVR used to do this through the action manifest and binding files (steamvr/*.json); the
// equivalent bindings now live in ProcessInput here: the "main" hand (right, or left with
// g_LeftHanded) fires, reloads and turns, the "off" hand moves, uses and opens the menu.
class VRInput
{
public:
	bool Init(CXGame* game, VROpenXR* xr);

	// reads the controllers and drives the game's actions; call once per frame while playing
	void ProcessInput();
	// reads the controllers without driving anything (while the menu owns them), so that a button
	// still held when the game resumes does not count as a fresh press
	void Poll() { UpdateActions(); }
	void ProcessInputOnFoot();
	void ProcessInputInVehicles();
	void ProcessInputBinoculars();

	void TriggerHaptics(int hand, float amplitude, float frequency, float duration);

	Matrix34 GetControllerTransform(int hand);
	bool IsControllerActive(int hand) const;

private:
	// the game's actions (what the SteamVR action manifest used to declare)
	enum Action
	{
		A_Menu,
		A_Use,
		A_Binoculars,
		A_GripLeft,        // physical left hand, whichever hand is dominant
		A_GripRight,
		A_ZoomIn,
		A_ZoomOut,
		A_Move,            // analog
		A_Turn,            // analog (continuous turn)
		A_SnapTurnLeft,
		A_SnapTurnRight,
		A_Sprint,
		A_Jump,
		A_Crouch,
		A_VehicleSteer,    // analog
		A_VehicleAccelerate,   // analog, only with vr_vehicle_alt_controls
		A_VehicleBrake,        // analog, only with vr_vehicle_alt_controls
		A_VehicleLeave,
		A_VehicleAttack,
		A_VehicleChangeView,
		A_VehicleChangeSeat,
		A_VehicleLights,
		A_VehicleReload,
		A_WeaponFire,
		A_WeaponReload,
		A_WeaponNext,
		A_WeaponGrenades,
		A_Count
	};

	struct ActionState
	{
		bool active = false;   // a controller that can produce this action is connected
		bool state = false;    // digital: pressed
		bool changed = false;  // digital: state differs from the previous frame
		float x = 0;           // analog
		float y = 0;
	};

	struct DoubleBindAction
	{
		Action action = A_Count;
		bool isPressed = false;
		float timeFirstPressed = 0;
	};

	// a thumbstick direction acting as a button (SteamVR "dpad" mode with sticky deadzone)
	struct Dpad
	{
		bool north = false, south = false, east = false, west = false;
	};

	CXGame* m_pGame = nullptr;
	VROpenXR* m_xr = nullptr;

	ActionState m_actions[A_Count];
	bool m_triggerDown[2] = {};   // trigger "click" with hysteresis, per physical hand
	bool m_squeezeDown[2] = {};   // grip "click" with hysteresis, per physical hand
	bool m_sprintLatch = false;   // stick click latched while the stick stays deflected
	Dpad m_dpad[2];               // thumbstick dpad state, per physical hand
	Matrix34 m_lastControllerTransform[2];

	DoubleBindAction m_defaultUse;
	DoubleBindAction m_defaultMenu;
	DoubleBindAction m_vehiclesChangeSeat;
	DoubleBindAction m_vehiclesReloadFireMode;
	DoubleBindAction m_weaponsReloadFireMode;
	DoubleBindAction m_weaponsNextDrop;
	DoubleBindAction m_weaponsGrenades;

	using TriggerFn = void (CXClient::*)(float value, XActivationEvent ae);

	void UpdateActions();
	void SetDigital(Action action, bool active, bool state);
	void SetAnalog(Action action, bool active, float x, float y);
	void UpdateDpad(int hand, const VROpenXR::HandState& state);

	void HandleBooleanAction(Action action, TriggerFn trigger, bool continuous = true);
	void HandleAnalogAction(Action action, int axis, TriggerFn trigger);
	float GetFloatValue(Action action, int axis = 0, bool *isActive = nullptr);

	void InitDoubleBindAction(DoubleBindAction& action, Action gameAction);
	void HandleDoubleBindAction(DoubleBindAction& action, TriggerFn shortPressTrigger, TriggerFn longPressTrigger, bool longContinuous = true);

	bool IsHandTouchingHead(int hand, float radius = 0.3f);
};
