#pragma once

#ifdef CONFIG_IDF_TARGET_ESP32S3
	#define MAX_GPIO 47
#else
	#define MAX_GPIO 39
#endif
#define MIN_PORT_EXPANDER_PIN 100
#define MAX_PORT_EXPANDER_PIN 115

static inline bool isGPIO(int32_t pin)
{
	return pin >= 0 && pin <= MAX_GPIO;
}

static inline bool isValidPin(int32_t pin)
{
	return (pin >= 0 && pin <= MAX_GPIO) || (pin >= MIN_PORT_EXPANDER_PIN && pin <= MAX_PORT_EXPANDER_PIN);
}



void Port_Init(void);
void Port_Cyclic(void);
bool Port_Read(const uint8_t _channel);
void Port_Write(const uint8_t _channel, const bool _newState, const bool _initGpio);
void Port_Exit(void);
