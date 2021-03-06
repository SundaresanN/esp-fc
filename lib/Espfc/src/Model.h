#ifndef _ESPFC_MODEL_H_
#define _ESPFC_MODEL_H_

#include <cstddef>
#include <cstdint>

#include "Debug.h"
#include "ModelConfig.h"
#include "ModelState.h"
#include "EEPROM.h"
#include "Logger.h"
#include "Math.h"
#include "EspGpio.h"
#include "EscDriver.h"

namespace Espfc {

class Model
{
  public:
    Model()
    {
      initialize();
    }

    void begin()
    {
      logger.begin();
      EEPROM.begin(1024);
      load();
      update();
    }

    void initialize()
    {
      config = ModelConfig();
      //config.brobot();
    }

    bool isActive(FlightMode mode) const
    {
      return state.modeMask & (1 << mode);
    }

    bool isSwitchActive(FlightMode mode) const
    {
      return state.modeMaskNew & (1 << mode);
    }

    bool hasChanged(FlightMode mode) const
    {
      return (state.modeMask & (1 << mode)) != (state.modeMaskPrev & (1 << mode));
    }

    bool isActive(Feature feature) const
    {
      return config.featureMask & feature;
    }

    bool isThrottleLow() const
    {
      return state.inputUs[AXIS_THRUST] < config.input.minCheck;
    }

    bool blackboxEnabled() const
    {
      return config.blackboxDev == 3 && config.blackboxPdenom > 0;
    }

    bool accelActive()
    {
      return config.accelDev != ACCEL_NONE;
    }

    bool magActive()
    {
      return config.magDev != MAG_NONE;
    }

    bool calibrationActive()
    {
      return state.sensorCalibration || state.accelBiasSamples || state.gyroBiasSamples;
    }

    void calibrate()
    {
      state.sensorCalibration = true;
      state.gyroBiasSamples  = 2 * state.gyroTimer.rate;
      if(accelActive())
      {
        state.accelBiasSamples = 2 * state.gyroTimer.rate;
        state.accelBias.z += 1.f;
      }
    }

    void finishCalibration()
    {
      if(state.sensorCalibration && state.accelBiasSamples == 0 && state.gyroBiasSamples == 0)
      {
        state.sensorCalibration = false;
        save();
      }
    }

    void update()
    {
      int gyroSyncMax = 4; // max 2khz
      if(accelActive()) gyroSyncMax = 8; // max 1khz
      if(config.magDev != MAG_NONE) gyroSyncMax = 16; // max 500hz

      config.gyroSync = std::max((int)config.gyroSync, gyroSyncMax); // max 1khz
      int gyroClock = 8000;
      if(config.gyroDlpf != GYRO_DLPF_256)
      {
        gyroClock = 1000;
        config.gyroSync = ((config.gyroSync + 7) / 8) * 8;
      }
      int gyroSampleRate = 8000 / config.gyroSync;

      // init timers
      // sample rate = clock / ( divider + 1)
      state.gyroDivider = (gyroClock / (gyroSampleRate + 1)) + 1;
      state.gyroTimer.setRate(gyroClock / state.gyroDivider);
      state.loopTimer.setRate(state.gyroTimer.rate, config.loopSync);
      state.mixerTimer.setRate(state.loopTimer.rate, config.mixerSync);
      state.actuatorTimer.setRate(25); // 25 hz
      state.telemetryTimer.setInterval(config.telemetryInterval * 1000);
      state.stats.timer.setRate(10);

      // configure calibration
      state.gyroBiasAlpha = 5.0f / state.gyroTimer.rate;
      state.accelBiasAlpha = 5.0f / state.gyroTimer.rate;
      state.gyroBiasSamples = 2 * state.gyroTimer.rate; // start gyro calibration

      // load sensor calibration data
      for(size_t i = 0; i <= AXIS_YAW; i++)
      {
        state.gyroBias.set(i, config.gyroBias[i] / 1000.0f);
        state.accelBias.set(i, config.accelBias[i] / 1000.0f);
        state.magCalibrationOffset.set(i, config.magCalibrationOffset[i] / 1000.0f);
        state.magCalibrationScale.set(i, config.magCalibrationScale[i] / 1000.0f);
      }

      for(size_t i = 0; i <= AXIS_YAW; i++)
      {
        state.gyroFilter[i].begin(config.gyroFilter, state.gyroTimer.rate);
        state.gyroNotch1Filter[i].begin(config.gyroNotch1Filter, state.gyroTimer.rate);
        state.gyroNotch2Filter[i].begin(config.gyroNotch2Filter, state.gyroTimer.rate);
        state.accelFilter[i].begin(config.accelFilter, state.gyroTimer.rate);
        state.magFilter[i].begin(config.magFilter, state.gyroTimer.rate);
      }

      // ensure disarmed pulses
      for(size_t i = 0; i < OUTPUT_CHANNELS; i++)
      {
        state.outputDisarmed[i] = config.output.channel[i].servo ? config.output.channel[i].neutral : config.output.minCommand; // ROBOT
      }

      config.output.protocol = ESC_PROTOCOL_SANITIZE(config.output.protocol);

      switch(config.output.protocol)
      {
        case ESC_PROTOCOL_BRUSHED:
          config.output.async = true;
          break;
        case ESC_PROTOCOL_DSHOT150:
        case ESC_PROTOCOL_DSHOT300:
        case ESC_PROTOCOL_DSHOT600:
        case ESC_PROTOCOL_DSHOT1200:
        case ESC_PROTOCOL_PROSHOT:
          config.output.async = false;
          break;
      }

      if(config.output.async)
      {
        // for async limit pwm rate
        switch(config.output.protocol)
        {
          case ESC_PROTOCOL_PWM:
            config.output.rate = constrain(config.output.rate, 50, 480);
            break;
          case ESC_PROTOCOL_ONESHOT125:
            config.output.rate = constrain(config.output.rate, 50, 2000);
            break;
          case ESC_PROTOCOL_ONESHOT42:
            config.output.rate = constrain(config.output.rate, 50, 4000);
            break;
          case ESC_PROTOCOL_BRUSHED:
          case ESC_PROTOCOL_MULTISHOT:
            config.output.rate = constrain(config.output.rate, 50, 8000);
            break;
          default:
            config.output.rate = constrain(config.output.rate, 50, 2000);
            break;
        }
      }
      else
      {
        // for synced and standard PWM limit loop rate and pwm pulse width
        if(config.output.protocol == ESC_PROTOCOL_PWM && gyroSampleRate > 500)
        {
          config.loopSync = std::max(config.loopSync, (int8_t)((gyroSampleRate + 499) / 500)); // align loop rate to lower than 500Hz
          int loopRate = gyroSampleRate / config.loopSync;
          if(loopRate > 480 && config.output.maxThrottle > 1950)
          {
            config.output.maxThrottle = 1940;
          }
        }
      }

      // configure serial ports
      uint32_t serialFunctionAllowedMask = SERIAL_FUNCTION_MSP | SERIAL_FUNCTION_BLACKBOX | SERIAL_FUNCTION_TELEMETRY_FRSKY;
      uint32_t featureAllowMask = FEATURE_RX_PPM | FEATURE_MOTOR_STOP | FEATURE_TELEMETRY;
      if(config.softSerialGuard)
      {
        featureAllowMask |= FEATURE_SOFTSERIAL;
      }
      if(config.serialRxGuard)
      {
        featureAllowMask |= FEATURE_RX_SERIAL;
        serialFunctionAllowedMask |= SERIAL_FUNCTION_RX_SERIAL;
      }
      config.featureMask = config.featureMask & featureAllowMask;

#if defined(ESP32)
      config.serial[SERIAL_UART_0].functionMask &= serialFunctionAllowedMask;
      config.serial[SERIAL_UART_1].functionMask &= serialFunctionAllowedMask;
      config.serial[SERIAL_UART_2].functionMask &= serialFunctionAllowedMask;
#elif defined(ESP8266)
      config.serial[SERIAL_UART_0].functionMask &= serialFunctionAllowedMask;
      config.serial[SERIAL_UART_1].functionMask &= serialFunctionAllowedMask;
      config.serial[SERIAL_SOFT_0].functionMask &= serialFunctionAllowedMask;
      //config.serial[SERIAL_SOFT_0].functionMask &= ~SERIAL_FUNCTION_RX_SERIAL;  // disallow
      //config.serial[SERIAL_SOFT_0].functionMask |= SERIAL_FUNCTION_RX_SERIAL; // force
#endif
      //config.featureMask |= FEATURE_RX_PPM; // force ppm
      //config.featureMask &= ~FEATURE_RX_PPM; // disallow ppm

      config.serial[SERIAL_UART_0].functionMask |= SERIAL_FUNCTION_MSP; // msp always enabled on uart0

      // only few beeper modes allowed
      config.buzzer.beeperMask &=
        1 << (BEEPER_GYRO_CALIBRATED - 1) |
        1 << (BEEPER_SYSTEM_INIT - 1) |
        1 << (BEEPER_RX_LOST - 1) |
        1 << (BEEPER_RX_SET - 1) |
        1 << (BEEPER_DISARMING - 1) |
        1 << (BEEPER_ARMING - 1) |
        1 << (BEEPER_BAT_LOW - 1);

      state.buzzer.beeperMask = config.buzzer.beeperMask;

      // configure PIDs
      float pidScale[] = { 1.f, 1.f, 1.f };
      if(config.mixerType == MIXER_GIMBAL)
      {
        pidScale[AXIS_YAW] = 0.2f; // ROBOT
        pidScale[AXIS_PITCH] = 20.f; // ROBOT
      }

      for(size_t i = 0; i <= AXIS_YAW; i++) // rpy
      {
        PidConfig * pc = &config.pid[i];
        state.innerPid[i].configure(
          pc->P * PTERM_SCALE * pidScale[i],
          pc->I * ITERM_SCALE * pidScale[i],
          pc->D * DTERM_SCALE * pidScale[i],
          config.itermWindupPointPercent / 100.0f,
          config.dtermSetpointWeight / 100.0f,
          1.0f
        );
        state.innerPid[i].dtermFilter.begin(config.dtermFilter, state.loopTimer.rate);
        state.innerPid[i].dtermNotchFilter.begin(config.dtermNotchFilter, state.loopTimer.rate);
        if(i == AXIS_YAW) state.innerPid[i].ptermFilter.begin(config.yawFilter, state.loopTimer.rate);
        else state.innerPid[i].ptermFilter.begin(); // no filter
      }

      for(size_t i = 0; i <= AXIS_YAW; i++)
      {
        PidConfig * pc = &config.pid[PID_LEVEL];
        state.outerPid[i].configure(
          pc->P * LEVEL_PTERM_SCALE,
          pc->I * LEVEL_ITERM_SCALE,
          pc->D * LEVEL_DTERM_SCALE,
          radians(config.angleRateLimit) * 0.05f,
          0,
          radians(config.angleRateLimit)
        );

        //state.outerPid[i].iLimit = 0.3f; // ROBOT
        //state.outerPid[i].dGamma = config.dtermSetpointWeight / 100.0f;  // ROBOT
        //state.outerPid[i].oLimit = 1.f;  // ROBOT

        state.outerPid[i].dtermFilter.begin(config.dtermFilter, state.loopTimer.rate);
        state.outerPid[i].dtermNotchFilter.begin(config.dtermNotchFilter, state.loopTimer.rate);
        state.outerPid[i].ptermFilter.begin(config.levelPtermFilter, state.loopTimer.rate);
      }
      state.customMixer = MixerConfig(config.customMixerCount, config.customMixes);

      //config.scaler[0].dimension = (ScalerDimension)(ACT_INNER_P | ACT_AXIS_PITCH); // ROBOT
      //config.scaler[1].dimension = (ScalerDimension)(ACT_INNER_P | ACT_AXIS_YAW);   // ROBOT
      //config.scaler[1].dimension = (ScalerDimension)(ACT_INNER_I | ACT_AXIS_PITCH); // ROBOT
      //config.scaler[2].dimension = (ScalerDimension)(ACT_INNER_D | ACT_AXIS_PITCH); // ROBOT
      //config.scaler[0].dimension = (ScalerDimension)(ACT_OUTER_P | ACT_AXIS_PITCH); // ROBOT
      //config.scaler[1].dimension = (ScalerDimension)(ACT_OUTER_I | ACT_AXIS_PITCH); // ROBOT
    }

    void preSave()
    {
      // store current sensor calibration
      for(size_t i = 0; i < 3; i++)
      {
        config.gyroBias[i] = lrintf(state.gyroBias[i] * 1000.0f);
        config.accelBias[i] = lrintf(state.accelBias[i] * 1000.0f);
        config.magCalibrationOffset[i] = lrintf(state.magCalibrationOffset[i] * 1000.0f);
        config.magCalibrationScale[i] = lrintf(state.magCalibrationScale[i] * 1000.0f);
      }
    }

    int load()
    {
      int addr = 0;
      uint8_t magic = EEPROM.read(addr++);
      if(EEPROM_MAGIC != magic)
      {
        logger.err().logln(F("EEPROM bad magic"));
        return -1;
      }

      uint8_t version = EEPROM.read(addr++);
      if(version != EEPROM_VERSION)
      {
        logger.err().logln(F("EEPROM wrong version"));
        return -1;
      }

      uint16_t size = 0;
      size = EEPROM.read(addr++);
      size |= EEPROM.read(addr++) << 8;
      if(size != sizeof(ModelConfig))
      {
        logger.info().logln(F("EEPROM size mismatch"));
      }

      size = std::min(size, (uint16_t)sizeof(ModelConfig));

      uint8_t * begin = reinterpret_cast<uint8_t*>(&config);
      uint8_t * end = begin + size;
      for(uint8_t * it = begin; it < end; ++it)
      {
        *it = EEPROM.read(addr++);
      }
      logger.info().logln(F("EEPROM loaded"));
      return 1;
    }

    void save()
    {
      preSave();
      write(config);
      logger.info().logln(F("EEPROM saved"));
    }

    void write(const ModelConfig& config)
    {
      int addr = 0;
      uint16_t size = sizeof(ModelConfig);
      EEPROM.write(addr++, EEPROM_MAGIC);
      EEPROM.write(addr++, EEPROM_VERSION);
      EEPROM.write(addr++, size & 0xFF);
      EEPROM.write(addr++, size >> 8);
      const uint8_t * begin = reinterpret_cast<const uint8_t*>(&config);
      const uint8_t * end = begin + sizeof(ModelConfig);
      for(const uint8_t * it = begin; it < end; ++it)
      {
        EEPROM.write(addr++, *it);
      }
      EEPROM.commit();
    }

    void reset()
    {
      initialize();
      save();
      update();
    }

    static const uint8_t EEPROM_MAGIC   = 0xA5;
    static const uint8_t EEPROM_VERSION = EEPROM_VERSION_NUM;

    ModelState state;
    ModelConfig config;
    Logger logger;
};

}

#endif
