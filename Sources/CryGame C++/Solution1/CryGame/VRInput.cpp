#include "StdAfx.h"
#include "VRInput.h"

#include "VRManager.h"
#include "WeaponClass.h"
#include "XPlayer.h"
#include "XVehicle.h"

namespace
{
	// the trigger acts as a button ("click") with hysteresis so a half-pulled trigger does not chatter
	const float kTriggerPress = 0.6f;
	const float kTriggerRelease = 0.4f;
	// grip, with hysteresis as well (Touch delivers a value, Index the grip force, WMR and Vive 0 or 1)
	const float kSqueezePress = 0.6f;
	const float kSqueezeRelease = 0.4f;
	// a latched stick click (sprint) lets go once the stick comes back this close to the center
	const float kSprintRelease = 0.3f;
	// thumbstick directions used as buttons, SteamVR "dpad" mode: deadzone_pct 80 for zoom/jump/crouch,
	// 75 for snap turning, and "sticky" - the direction stays active until the stick comes back
	const float kDpadVertical = 0.8f;
	const float kDpadHorizontal = 0.75f;
	const float kDpadRelease = 0.4f;

	// SteamVR "exponent 2": the stick's deflection is squared, the direction stays the same
	void ApplyExponent(float& x, float& y)
	{
		float len = sqrtf(x * x + y * y);
		if (len > 1.0f)
		{
			x /= len;
			y /= len;
			len = 1.0f;
		}
		x *= len;
		y *= len;
	}
}

bool VRInput::Init(CXGame* game, VROpenXR* xr)
{
	m_pGame = game;
	m_xr = xr;

	for (int hand = 0; hand < 2; ++hand)
		m_lastControllerTransform[hand] = Matrix34::CreateIdentity();

	InitDoubleBindAction(m_defaultMenu, A_Menu);
	InitDoubleBindAction(m_defaultUse, A_Use);
	InitDoubleBindAction(m_vehiclesChangeSeat, A_VehicleChangeSeat);
	InitDoubleBindAction(m_vehiclesReloadFireMode, A_VehicleReload);
	InitDoubleBindAction(m_weaponsReloadFireMode, A_WeaponReload);
	InitDoubleBindAction(m_weaponsNextDrop, A_WeaponNext);
	InitDoubleBindAction(m_weaponsGrenades, A_WeaponGrenades);

	return m_xr != nullptr && m_xr->IsInitialized() && m_xr->HasInput();
}

void VRInput::SetDigital(Action action, bool active, bool state)
{
	ActionState& a = m_actions[action];
	state = active && state;
	a.changed = active && state != a.state;
	a.active = active;
	a.state = state;
	a.x = state ? 1.0f : 0.0f;
	a.y = 0;
}

void VRInput::SetAnalog(Action action, bool active, float x, float y)
{
	ActionState& a = m_actions[action];
	a.active = active;
	a.x = active ? x : 0.0f;
	a.y = active ? y : 0.0f;
	a.state = false;
	a.changed = false;
}

void VRInput::UpdateDpad(int hand, const VROpenXR::HandState& state)
{
	Dpad& pad = m_dpad[hand];
	const float x = state.stickX;
	const float y = state.stickY;
	const float len = sqrtf(x * x + y * y);

	if (!state.active || len < kDpadRelease)
	{
		pad = Dpad();
		return;
	}

	// a direction switches on when the stick is deflected far enough into its 90 degree sector and stays
	// on, alone, until the stick comes back to the center (or crosses over to the opposite side)
	if (y < 0) pad.north = false;
	if (y > 0) pad.south = false;
	if (x < 0) pad.east = false;
	if (x > 0) pad.west = false;
	if (pad.north || pad.south || pad.east || pad.west)
		return;

	const bool vertical = fabsf(y) >= fabsf(x);
	if (len >= kDpadVertical && vertical)
	{
		if (y > 0) pad.north = true; else pad.south = true;
	}
	if (len >= kDpadHorizontal && !vertical)
	{
		if (x > 0) pad.east = true; else pad.west = true;
	}
}

void VRInput::UpdateActions()
{
	const bool leftHanded = m_pGame->g_LeftHanded->GetIVal() != 0;
	const int mainHand = leftHanded ? VROpenXR::Hand_Left : VROpenXR::Hand_Right;
	const int offHand = leftHanded ? VROpenXR::Hand_Right : VROpenXR::Hand_Left;

	for (int hand = 0; hand < 2; ++hand)
	{
		const VROpenXR::HandState& h = m_xr->GetHand(hand);
		if (!h.active)
		{
			m_triggerDown[hand] = false;
			m_squeezeDown[hand] = false;
			m_dpad[hand] = Dpad();
			continue;
		}
		m_triggerDown[hand] = h.trigger > (m_triggerDown[hand] ? kTriggerRelease : kTriggerPress);
		m_squeezeDown[hand] = h.squeeze > (m_squeezeDown[hand] ? kSqueezeRelease : kSqueezePress);
		UpdateDpad(hand, h);
	}

	const VROpenXR::HandState& main = m_xr->GetHand(mainHand);
	const VROpenXR::HandState& off = m_xr->GetHand(offHand);
	const VROpenXR::HandState& left = m_xr->GetHand(VROpenXR::Hand_Left);
	const VROpenXR::HandState& right = m_xr->GetHand(VROpenXR::Hand_Right);
	const bool mainTrigger = m_triggerDown[mainHand];
	const bool offTrigger = m_triggerDown[offHand];
	const Dpad& mainPad = m_dpad[mainHand];

	float moveX = off.stickX, moveY = off.stickY;
	// SteamVR "sticky click": a click of the move stick keeps sprinting for as long as the stick stays
	// deflected, so the thumb does not have to hold the stick down while running
	const float moveDeflection = sqrtf(moveX * moveX + moveY * moveY);
	m_sprintLatch = off.active && (off.stickClick || (m_sprintLatch && moveDeflection > kSprintRelease));
	ApplyExponent(moveX, moveY);
	float turnX = main.stickX, turnY = main.stickY;
	ApplyExponent(turnX, turnY);

	// default set (bindings_touch.json / bindings_knuckles.json, mirrored for left-handed players)
	SetDigital(A_Menu, off.active, off.secondary);                 // Y / B / menu button
	SetDigital(A_Use, off.active, offTrigger);
	SetDigital(A_Binoculars, off.active, off.primary);             // X / A
	SetDigital(A_GripLeft, left.active, m_squeezeDown[VROpenXR::Hand_Left]);
	SetDigital(A_GripRight, right.active, m_squeezeDown[VROpenXR::Hand_Right]);
	SetDigital(A_ZoomIn, main.active, mainPad.north);
	SetDigital(A_ZoomOut, main.active, mainPad.south);

	// move set
	SetAnalog(A_Move, off.active, moveX, moveY);
	SetAnalog(A_Turn, main.active, turnX, turnY);
	SetDigital(A_SnapTurnLeft, main.active, mainPad.west);
	SetDigital(A_SnapTurnRight, main.active, mainPad.east);
	SetDigital(A_Sprint, off.active, m_sprintLatch);
	SetDigital(A_Jump, main.active, mainPad.north);
	SetDigital(A_Crouch, main.active, mainPad.south);

	// vehicles set; vr_vehicle_alt_controls selects the "vehicles alt" binding: triggers accelerate and
	// brake, the face buttons take over attack/leave, the main stick click toggles the lights
	const bool alt = gVR->vr_vehicle_alt_controls != 0;
	SetAnalog(A_VehicleSteer, off.active, moveX, moveY);
	SetAnalog(A_VehicleAccelerate, alt && main.active, main.trigger, 0);
	SetAnalog(A_VehicleBrake, alt && off.active, off.trigger, 0);
	SetDigital(A_VehicleLeave, off.active, alt ? off.primary : offTrigger);
	SetDigital(A_VehicleAttack, main.active, alt ? main.primary : mainTrigger);
	SetDigital(A_VehicleChangeView, off.active, off.stickClick || off.padClick);
	SetDigital(A_VehicleChangeSeat, main.active, main.secondary);
	SetDigital(A_VehicleLights, alt ? main.active : off.active, alt ? main.stickClick : off.primary);
	SetDigital(A_VehicleReload, main.active, alt ? main.secondary : main.primary);

	// weapons set
	SetDigital(A_WeaponFire, main.active, mainTrigger);
	SetDigital(A_WeaponReload, main.active, main.primary);
	SetDigital(A_WeaponNext, main.active, main.stickClick || main.padClick);
	SetDigital(A_WeaponGrenades, main.active, main.secondary);
}

void VRInput::ProcessInput()
{
	UpdateActions();

	if (!m_pGame->GetClient())
		return;

	if (m_pGame->IsCutSceneActive())
	{
		HandleDoubleBindAction(m_defaultMenu, &CXClient::TriggerMenu, &CXClient::StopCutScene);
	}
	else
	{
		HandleDoubleBindAction(m_defaultMenu, &CXClient::TriggerMenu, &CXClient::TriggerScoreBoard);
	}
	if (gVR->vr_snap_turn_amount == 0 || gVR->IsDrivingVehicleInCinemaMode())
	{
		HandleAnalogAction(A_Turn, 0, &CXClient::TriggerTurnLR);
		if (gVR->IsDrivingVehicleInCinemaMode())
			HandleAnalogAction(A_Turn, 1, &CXClient::TriggerTurnUD);
	}
	else
	{
		HandleBooleanAction(A_SnapTurnLeft, &CXClient::TriggerSnapTurnLeft, false);
		HandleBooleanAction(A_SnapTurnRight, &CXClient::TriggerSnapTurnRight, false);
	}

	CPlayer* player = m_pGame->GetLocalPlayer();
	if (m_pGame->AreBinocularsActive())
		ProcessInputBinoculars();
	else if (player && player->GetVehicle() && player->GetVehicle()->GetType() != VHT_PARAGLIDER)  // paraglider works better with on-foot controls
		ProcessInputInVehicles();
	else
		ProcessInputOnFoot();
}

void VRInput::ProcessInputOnFoot()
{
	int mainHand = m_pGame->g_LeftHanded->GetIVal() != 0 ? 0 : 1;
	int offHand = m_pGame->g_LeftHanded->GetIVal() != 0 ? 1 : 0;
	CPlayer* player = m_pGame->GetLocalPlayer();
	if (player && player->GetSelectedWeapon())
	{
		CWeaponClass* weapon = player->GetSelectedWeapon();
		if (weapon->IsZoomActive() && (!IsHandTouchingHead(mainHand, 0.35f) || !player->IsTwoHandedModeActive()))
		{
			player->GetEntity()->SendScriptEvent(ScriptEvent_ZoomToggle, 2);
		}
		else if (player->IsTwoHandedModeActive() && !player->m_stats.running && !player->IsSwimming() && !weapon->IsZoomActive() && IsHandTouchingHead(mainHand, 0.3f) && weapon->HasActualScope())
		{
			player->GetEntity()->SendScriptEvent(ScriptEvent_ZoomToggle, 1);
		}
	}

	if (player && player->IsWeaponZoomActive())
	{
		HandleBooleanAction(A_ZoomIn, &CXClient::TriggerZoomIn, false);
		HandleBooleanAction(A_ZoomOut, &CXClient::TriggerZoomOut, false);
	}
	else
	{
		if (IsHandTouchingHead(offHand))
		{
			// if touching head, toggle flashlight or thermal vision
			HandleDoubleBindAction(m_defaultUse, &CXClient::TriggerFlashlight, &CXClient::TriggerItem1, false);
		}
		else
		{
			// otherwise, normal use
			HandleBooleanAction(A_Use, &CXClient::TriggerUse, false);
			m_defaultUse.isPressed = false;
		}
		HandleBooleanAction(A_Binoculars, &CXClient::TriggerItem0, false);
		HandleBooleanAction(A_Crouch, &CXClient::TriggerMoveModeSwitch, false);
		HandleBooleanAction(A_Jump, &CXClient::TriggerJump, false);
		HandleDoubleBindAction(m_weaponsNextDrop, &CXClient::TriggerNextWeapon, &CXClient::TriggerDropWeapon, false);
		HandleDoubleBindAction(m_weaponsGrenades, &CXClient::CycleGrenade, &CXClient::TriggerFireGrenade, false);
		HandleBooleanAction(A_Sprint, &CXClient::TriggerRunSprint);
	}

	HandleAnalogAction(A_Move, 0, &CXClient::TriggerMoveLR);
	HandleAnalogAction(A_Move, 1, &CXClient::TriggerMoveFB);
	HandleBooleanAction(A_WeaponFire, &CXClient::TriggerFire0);
	HandleDoubleBindAction(m_weaponsReloadFireMode, &CXClient::TriggerReload, &CXClient::TriggerFireMode, false);
	HandleBooleanAction(A_GripLeft, &CXClient::TriggerLeftGrip);
	HandleBooleanAction(A_GripRight, &CXClient::TriggerRightGrip);
}

void VRInput::ProcessInputInVehicles()
{
	CPlayer* player = m_pGame->GetLocalPlayer();
	CVehicle* vehicle = player->GetVehicle();
	if (vehicle->GetUserInState(CPlayer::PVS_DRIVER) == player)
	{
		HandleAnalogAction(A_VehicleSteer, 0, &CXClient::TriggerMoveLR);
		HandleBooleanAction(A_VehicleLights, &CXClient::TriggerFlashlight, false);
		HandleBooleanAction(A_VehicleAttack, &CXClient::TriggerFire0);

		// combine accelerate/brake to movement value
		bool isAccelActive = false, isBrakeActive = false;
		float accel = GetFloatValue(A_VehicleAccelerate, 0, &isAccelActive);
		float brake = GetFloatValue(A_VehicleBrake, 0, &isBrakeActive);
		float move = accel - brake;

		if (isAccelActive && isBrakeActive)
			m_pGame->GetClient()->TriggerMoveFB(move, XActivationEvent());
		else
			HandleAnalogAction(A_VehicleSteer, 1, &CXClient::TriggerMoveFB);

		if (player->GetSelectedWeapon() && player->GetSelectedWeapon()->GetName() != vehicle->GetWeaponName(CPlayer::PVS_DRIVER))
		{
			// using own weapon - allow to reload and grab with second hand
			HandleDoubleBindAction(m_vehiclesReloadFireMode, &CXClient::TriggerReload, &CXClient::TriggerFireMode, false);
			HandleDoubleBindAction(m_weaponsNextDrop, &CXClient::TriggerNextWeapon, &CXClient::TriggerDropWeapon, false);
			HandleBooleanAction(A_GripLeft, &CXClient::TriggerLeftGrip);
			HandleBooleanAction(A_GripRight, &CXClient::TriggerRightGrip);
		}
	}
	else
	{
		HandleBooleanAction(A_WeaponFire, &CXClient::TriggerFire0);
		HandleDoubleBindAction(m_weaponsReloadFireMode, &CXClient::TriggerReload, &CXClient::TriggerFireMode, false);
		HandleDoubleBindAction(m_weaponsNextDrop, &CXClient::TriggerNextWeapon, &CXClient::TriggerDropWeapon, false);
		HandleBooleanAction(A_GripLeft, &CXClient::TriggerLeftGrip);
		HandleBooleanAction(A_GripRight, &CXClient::TriggerRightGrip);
	}

	HandleBooleanAction(A_VehicleLeave, &CXClient::TriggerUse, false);
	HandleBooleanAction(A_VehicleChangeView, &CXClient::TriggerChangeView, false);
	HandleDoubleBindAction(m_vehiclesChangeSeat, &CXClient::TriggerRunSprint, &CXClient::TriggerFireMode, false);

	// process some of the default actions to prevent them from immediately triggering when exiting the vehicle
	HandleBooleanAction(A_Binoculars, &CXClient::NoOp, false);
	HandleBooleanAction(A_Use, &CXClient::NoOp, false);
	HandleBooleanAction(A_Crouch, &CXClient::NoOp, false);
	HandleBooleanAction(A_Jump, &CXClient::NoOp, false);
	HandleBooleanAction(A_Sprint, &CXClient::NoOp, false);
}

void VRInput::ProcessInputBinoculars()
{
	HandleBooleanAction(A_Use, &CXClient::TriggerUse, false);
	HandleBooleanAction(A_Binoculars, &CXClient::TriggerItem0, false);
	HandleBooleanAction(A_ZoomIn, &CXClient::TriggerZoomIn, false);
	HandleBooleanAction(A_ZoomOut, &CXClient::TriggerZoomOut, false);
	HandleAnalogAction(A_Move, 0, &CXClient::TriggerMoveLR);
	HandleAnalogAction(A_Move, 1, &CXClient::TriggerMoveFB);
}

void VRInput::TriggerHaptics(int hand, float amplitude, float frequency, float duration)
{
	if (m_xr)
		m_xr->ApplyHaptic(hand, amplitude, frequency, duration);
}

Matrix34 VRInput::GetControllerTransform(int hand)
{
	hand = clamp_tpl(hand, 0, 1);

	XrPosef pose;
	if (m_xr && m_xr->GetHandPose(hand, false, pose))
	{
		// the grip pose has a peculiar orientation that we need to fix
		Matrix33 correction = Matrix33::CreateRotationX(gf_PI/2);
		m_lastControllerTransform[hand] = XrPoseToFarCry(pose) * correction;
	}
	// a controller that is not tracked right now keeps its last known place
	return m_lastControllerTransform[hand];
}

bool VRInput::IsControllerActive(int hand) const
{
	return m_xr && m_xr->GetHand(hand).active;
}

void VRInput::HandleBooleanAction(Action action, TriggerFn trigger, bool continuous)
{
	const ActionState& a = m_actions[action];
	if (a.active && a.state && (continuous || a.changed))
	{
		(m_pGame->GetClient()->*trigger)(1.f, XActivationEvent());
	}
}

void VRInput::HandleAnalogAction(Action action, int axis, TriggerFn trigger)
{
	const ActionState& a = m_actions[action];
	if (!a.active)
		return;

	float value = axis == 0 ? a.x : a.y;
	(m_pGame->GetClient()->*trigger)(value, XActivationEvent());
}

float VRInput::GetFloatValue(Action action, int axis, bool* isActive)
{
	const ActionState& a = m_actions[action];
	if (isActive != nullptr)
		*isActive = a.active;
	if (!a.active)
		return 0.f;

	return axis == 0 ? a.x : a.y;
}

void VRInput::InitDoubleBindAction(DoubleBindAction& action, Action gameAction)
{
	action.action = gameAction;
	action.isPressed = false;
	action.timeFirstPressed = 0;
}

void VRInput::HandleDoubleBindAction(DoubleBindAction& action, TriggerFn shortPressTrigger, TriggerFn longPressTrigger, bool longContinuous)
{
	const ActionState& a = m_actions[action.action];
	if (!a.active)
	{
		action.isPressed = false;
		action.timeFirstPressed = 0;
		return;
	}

	if (a.state && a.changed)
	{
		action.isPressed = true;
		action.timeFirstPressed = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime();
	}

	if (a.state && action.isPressed)
	{
		if (action.timeFirstPressed == 0)
		{
			// long press already active
			if (longContinuous)
				(m_pGame->GetClient()->*longPressTrigger)(1.f, XActivationEvent());
		}
		else
		{
			float delta = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime() - action.timeFirstPressed;
			if (delta >= gVR->vr_button_long_press_time)
			{
				action.timeFirstPressed = 0;  // mark long press active
				(m_pGame->GetClient()->*longPressTrigger)(1.f, XActivationEvent());
			}
		}
	}

	if (!a.state && action.isPressed)
	{
		if (action.timeFirstPressed != 0)
		{
			// enable short press action on release since long press was not active
			(m_pGame->GetClient()->*shortPressTrigger)(1.f, XActivationEvent());
		}

		action.isPressed = false;
		action.timeFirstPressed = 0;
	}
}

bool VRInput::IsHandTouchingHead(int hand, float radius)
{
	Matrix34 hmdTransform = gVR->GetHmdTransform();
	Vec3 hmdPos = hmdTransform.GetTranslation();
	hmdPos -= hmdTransform.GetForward() * 0.1f; // get a bit closer to the player head's centre
	Matrix34 controllerTransform = gVR->GetControllerTransform(hand);
	Vec3 controllerPos = controllerTransform.GetTranslation();
	return controllerPos.GetDistance(hmdPos) <= radius;
}
