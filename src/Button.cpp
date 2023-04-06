#include <Arduino.h>
#include "settings.h"
#include "Log.h"
#include "Button.h"
#include "Cmd.h"
#include "Port.h"
#include "System.h"
#include "values.h"
#include <stdint.h>

bool gButtonInitComplete = false;

// Only enable those buttons that are not disabled (99 or >115)
// 0 -> 39: GPIOs
// 100 -> 115: Port-expander
t_button gButtons[7];         // next + prev + pplay + rotEnc + button4 + button5 + dummy-button
uint8_t gShutdownButton = 99; // Helper used for Neopixel: stores button-number of shutdown-button
uint16_t gLongPressTime = 0;

static volatile SemaphoreHandle_t Button_TimerSemaphore;

hw_timer_t *Button_Timer = NULL;
static void IRAM_ATTR onTimer();
static void Button_DoButtonActions(void);

static const int32_t ButtonPins[] = {
	NEXT_BUTTON, PREVIOUS_BUTTON, PAUSEPLAY_BUTTON, ROTARYENCODER_BUTTON, BUTTON_4, BUTTON_5
};

static const uint32_t ButtonShortActions[] = {
	BUTTON_0_SHORT, BUTTON_1_SHORT, BUTTON_2_SHORT, BUTTON_3_SHORT, BUTTON_4_SHORT, BUTTON_5_SHORT
};

static const uint32_t ButtonLongActions[] = {
	BUTTON_0_LONG, BUTTON_1_LONG, BUTTON_2_LONG, BUTTON_3_LONG, BUTTON_4_LONG, BUTTON_5_LONG
};

static const uint32_t ButtonMultiActions[] = {
BUTTON_MULTI_01, BUTTON_MULTI_02, BUTTON_MULTI_03, BUTTON_MULTI_04, BUTTON_MULTI_05,
                 BUTTON_MULTI_12, BUTTON_MULTI_13, BUTTON_MULTI_14, BUTTON_MULTI_15,
                                  BUTTON_MULTI_23, BUTTON_MULTI_24, BUTTON_MULTI_25,
                                                   BUTTON_MULTI_34, BUTTON_MULTI_35,
                                                                    BUTTON_MULTI_45
};

#define NUM_BUTTONS (sizeof(ButtonPins)/sizeof(ButtonPins[0]))

static_assert(NUM_BUTTONS == sizeof(ButtonShortActions)/sizeof(ButtonShortActions[0]), "ButtonShortActions must be same length as ButtonPins");
static_assert(NUM_BUTTONS == sizeof(ButtonLongActions)/sizeof(ButtonLongActions[0]), "ButtonLongActions must be same length as ButtonPins");
static_assert((NUM_BUTTONS * (NUM_BUTTONS - 1)) / 2 == sizeof(ButtonMultiActions)/sizeof(ButtonMultiActions[0]), "ButtonMultiActions must contain all permutations");

void Button_Init() {
	#if (WAKEUP_BUTTON >= 0 && WAKEUP_BUTTON <= MAX_GPIO)
		if (ESP_ERR_INVALID_ARG == esp_sleep_enable_ext0_wakeup((gpio_num_t)WAKEUP_BUTTON, 0)) {
			snprintf(Log_Buffer, Log_BufferLength, "%s (GPIO: %u)", (char *) FPSTR(wrongWakeUpGpio), WAKEUP_BUTTON);
			Log_Println(Log_Buffer, LOGLEVEL_ERROR);
		}
	#endif

	// Activate internal pullups for all enabled buttons connected to GPIOs
	for (uint32_t i = 0; i < NUM_BUTTONS; i++)
	{
		#ifdef NEOPIXEL_ENABLE // Try to find button that is used for shutdown via longpress-action (only necessary for Neopixel)
			if (isValidPin(ButtonPins[i]) && ButtonLongActions[i] == CMD_SLEEPMODE) {
				gShutdownButton = i;
			}
		#endif
		if (isGPIO(ButtonPins[i])) {
			pinMode(ButtonPins[i], INPUT_PULLUP);
		}
	}

	// Create 1000Hz-HW-Timer (currently only used for buttons)
	Button_TimerSemaphore = xSemaphoreCreateBinary();
	Button_Timer = timerBegin(0, 240, true); // Prescaler: CPU-clock in MHz
	timerAttachInterrupt(Button_Timer, &onTimer, true);
	timerAlarmWrite(Button_Timer, 10000, true); // 100 Hz
	timerAlarmEnable(Button_Timer);
}

// If timer-semaphore is set, read buttons (unless controls are locked)
void Button_Cyclic() {
	if (xSemaphoreTake(Button_TimerSemaphore, 0) == pdTRUE) {
		unsigned long currentTimestamp = millis();
		#ifdef PORT_EXPANDER_ENABLE
			Port_Cyclic();
		#endif

		if (System_AreControlsLocked()) {
			return;
		}

		// Buttons can be mixed between GPIO and port-expander.
		// But at the same time only one of them can be for example NEXT_BUTTON
		for (uint32_t i = 0; i < NUM_BUTTONS; i++)
		{
			// Invalid pin numbers are always reported as not pressed by Port_Read
			gButtons[i].currentState = Port_Read(ButtonPins[i]);
		}

		snprintf(Log_Buffer, Log_BufferLength, "%d %d %d %d", gButtons[0].currentState, gButtons[1].currentState, gButtons[2].currentState, gButtons[3].currentState);
			Log_Println(Log_Buffer, LOGLEVEL_DEBUG);


		// Iterate over all buttons in struct-array
		for (uint8_t i = 0; i < sizeof(gButtons) / sizeof(gButtons[0]); i++) {
			if (gButtons[i].currentState != gButtons[i].lastState && currentTimestamp - gButtons[i].lastPressedTimestamp > buttonDebounceInterval) {
				if (!gButtons[i].currentState) {
					gButtons[i].isPressed = true;
					gButtons[i].lastPressedTimestamp = currentTimestamp;
					if (!gButtons[i].firstPressedTimestamp) {
						gButtons[i].firstPressedTimestamp = currentTimestamp;
					}
				} else {
					gButtons[i].isReleased = true;
					gButtons[i].lastReleasedTimestamp = currentTimestamp;
					gButtons[i].firstPressedTimestamp = 0;
				}
			}
			gButtons[i].lastState = gButtons[i].currentState;
		}
	}
	gButtonInitComplete = true;
	Button_DoButtonActions();
}

// Do corresponding actions for all buttons
void Button_DoButtonActions() {
	uint32_t actionIndex = 0;
	for (uint32_t firstButton = 0; firstButton < NUM_BUTTONS; firstButton++) {
		for (uint32_t secondButton = firstButton + 1; secondButton < NUM_BUTTONS; secondButton++) {
			if (gButtons[firstButton].isPressed && gButtons[secondButton].isPressed) {
				gButtons[firstButton].isPressed = false;
				gButtons[secondButton].isPressed = false;
				Cmd_Action(ButtonMultiActions[actionIndex]);
				return; // Stop processing after first action
			}
			actionIndex++;
		}
	}
	for (uint8_t i = 0; i <= NUM_BUTTONS; i++) {
		if (gButtons[i].isPressed) {
			uint8_t Cmd_Long = ButtonLongActions[i];
			if (gButtons[i].lastReleasedTimestamp > gButtons[i].lastPressedTimestamp) {
				if (gButtons[i].lastReleasedTimestamp - gButtons[i].lastPressedTimestamp < intervalToLongPress) {
					Cmd_Action(ButtonShortActions[i]);
				} else {
					// if not volume buttons than start action after button release
					if (Cmd_Long != CMD_VOLUMEUP && Cmd_Long != CMD_VOLUMEDOWN) {
						Cmd_Action(Cmd_Long);
					}
				}

				gButtons[i].isPressed = false;
			} else if (Cmd_Long == CMD_VOLUMEUP || Cmd_Long == CMD_VOLUMEDOWN) {
				unsigned long currentTimestamp = millis();

				// only start action if intervalToLongPress has been reached
				if (currentTimestamp - gButtons[i].lastPressedTimestamp > intervalToLongPress) {

					// calculate remainder
					uint16_t remainder = (currentTimestamp - gButtons[i].lastPressedTimestamp) % intervalToLongPress;

					// trigger action if remainder rolled over
					if (remainder < gLongPressTime) {
						Cmd_Action(Cmd_Long);
					}

					gLongPressTime = remainder;
				}
			}
		}
	}
}

void IRAM_ATTR onTimer() {
	xSemaphoreGiveFromISR(Button_TimerSemaphore, NULL);
}
