#if defined(ESP8266)

#include "Esp8266EscDriver.h"

#include "EspGpio.h"
#include <algorithm>

// uart activity brakes timer0 events, lets not use it

static inline void timer_write(uint32_t ticks)
{
  //timer0_write(ESP.getCycleCount() + ticks);
  timer1_write(ticks);
}

static inline void delay_ticks(uint32_t ticks)
{
  if(ticks < 30) return;
  uint32_t end = ESP.getCycleCount() + ticks - 20;
  while(ESP.getCycleCount() < end)
  {
    __asm__ volatile ("nop");
  };
}

int Esp8266EscDriver::attach(size_t channel, int pin, int pulse)
{
  if(channel < 0 || channel >= ESC_CHANNEL_COUNT) return 0;
  _buffer[channel].pin = pin;
  _buffer[channel].pulse = usToTicks(pulse);
  pinMode(pin, OUTPUT);
  return 1;
}

int Esp8266EscDriver::write(size_t channel, int pulse)
{
  if(channel < 0 || channel >= ESC_CHANNEL_COUNT) return 0;
  _buffer[channel].pulse = usToTicks(pulse);
  return 1;
}

void Esp8266EscDriver::apply()
{
  if(_async || _isr_busy == true) return;
  handle_isr();
}

void Esp8266EscDriver::handle_isr(void)
{
  if(!_instance) return;
  switch(_instance->_protocol)
  {
    //case ESC_PROTOCOL_ONESHOT42:
    case ESC_PROTOCOL_MULTISHOT:
      _instance->handle_serial();
      break;
    default:
      _instance->handle();
      break;
  }
}

// update timer or do active waiting
static bool _wait_or_yield(uint32_t ticks) ICACHE_RAM_ATTR;
static bool _wait_or_yield(uint32_t ticks)
{
  if(ticks > Esp8266EscDriver::PULSE_COMPENSATION)
  {
    timer_write(ticks - Esp8266EscDriver::PULSE_COMPENSATION + 20);
    return true;
  }
  delay_ticks(ticks);
  return false;
}

// enable all active pins, return first or end
static const Esp8266EscDriver::Slot * _activate_all(const Esp8266EscDriver::Slot * it, const Esp8266EscDriver::Slot * end);
static const Esp8266EscDriver::Slot * _activate_all(const Esp8266EscDriver::Slot * it, const Esp8266EscDriver::Slot * end)
{
  const Esp8266EscDriver::Slot * first = NULL;
  for(; it != end; it++)
  {
    if(!it->active()) continue;
    if(it->pulse <= 80) continue; // less than 1us(80 ticks) for brushed PWM (80 for timer1)
    if(!first) first = it;
    EspGpio::digitalWrite(it->pin, HIGH);
  }
  return first ? first : end;
}

void Esp8266EscDriver::handle_serial(void)
{
  static const Slot * en = end();
  static const Slot * it = NULL;

  if(!it)
  {
    _isr_busy = true;
    _space = _interval;
    std::copy(_buffer, _buffer + ESC_CHANNEL_COUNT, _slots);
    const Slot * first = NULL;
    for(it = begin(); it != en; ++it)
    {
      if(!it->active()) continue;
      if(!first) first = it;
      _space -= it->pulse;
    }
    it = first ? first : en;

    if(it != en)
    {
      EspGpio::digitalWrite(it->pin, HIGH);
      timer_write(it->pulse - 180);
      return;
    }
  }

  while(it != en)
  {
    EspGpio::digitalWrite(it->pin, LOW);

    it++;
    if(it == en) break;
    if(!it->active()) continue;

    EspGpio::digitalWrite(it->pin, HIGH);
    timer_write(it->pulse - 150);
    return;
  }

  // finish cycle
  it = NULL;
  _isr_busy = false;

  if(_async)
  {
    timer_write(_space);
    return;
  }
}

void Esp8266EscDriver::handle(void)
{
  static const Slot * en = end();
  static const Slot * it = NULL;

  // start cycle
  if(!it)
  {
    _isr_busy = true;
    commit();

    it = _activate_all(begin(), en);
    if(it != en)
    {
      if(_wait_or_yield(it->diff)) return;
    }
  }

  // turn off pins and suppress similar pulses
  while(it != en)
  {
    EspGpio::digitalWrite(it->pin, LOW);
    it++;
    if(it == en) break;
    if(!it->active()) break;
    if(_wait_or_yield(it->diff)) return;
  }

  // finish cycle
  it = NULL;
  _isr_busy = false;

  // trigger next cycle in async mode
  if(_async)
  {
    timer_write(_space);
    return;
  }
}

void Esp8266EscDriver::commit()
{
  Slot tmp[ESC_CHANNEL_COUNT];
  std::copy(_buffer, _buffer + ESC_CHANNEL_COUNT, tmp);
  std::sort(tmp, tmp + ESC_CHANNEL_COUNT);
  Slot * end = tmp + ESC_CHANNEL_COUNT;
  Slot * prev = NULL;
  for(Slot * it = tmp; it != end; ++it)
  {
    if(!it->active()) break;
    if(!prev) it->diff = it->pulse;
    else it->diff = it->pulse - prev->pulse;
    prev = it;
  }
  _space = prev ? _interval - prev->pulse : _interval; // after loop prev is the last item with longest pulse
  std::copy(tmp, tmp + ESC_CHANNEL_COUNT, _slots);
}

Esp8266EscDriver::Esp8266EscDriver(): _protocol(ESC_PROTOCOL_PWM), _async(true), _rate(50), _isr_busy(false)
{
  _interval_us = 1000000L / _rate;
  _interval = usToTicksReal(_interval_us);
  for(size_t i = 0; i < ESC_CHANNEL_COUNT; ++i)
  {
    _slots[i] = Slot();
    _buffer[i] = Slot();
  }
}

int Esp8266EscDriver::begin(EscProtocol protocol, bool async, int16_t rate)
{
  _protocol = ESC_PROTOCOL_SANITIZE(protocol);
  _async = _protocol == ESC_PROTOCOL_BRUSHED ? true : async; // ignore async for brushed
  _rate = constrain(rate, 50, 4000);
  _interval_us = 1000000L / _rate;
  _interval = usToTicksReal(_interval_us);
  if(!_instance)
  {
    _instance = this;
    noInterrupts();
    //timer0_isr_init();
    //timer0_attachInterrupt(Esp8266EscDriver::handle_isr);
    timer1_isr_init();
    timer1_attachInterrupt(Esp8266EscDriver::handle_isr);
    timer1_enable(TIM_DIV1, TIM_EDGE, TIM_SINGLE);
    interrupts();
    if(_async) timer_write(_interval);
  }
  return 1;
}

Esp8266EscDriver * Esp8266EscDriver::_instance = NULL;

#endif // ESP8266