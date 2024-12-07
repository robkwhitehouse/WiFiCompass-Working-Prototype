/*
 * Simple class for a 3 colour LED driver
 * They have internal Red Green and Blue LEDS and 4 connection pins
 * They can be common cathode, or common anode. The former has common GND pin
 * The latter has a common VCC pin
 *
 * This class offers the following methods;
 *    TricolourLED() Constructor - must specifiy 3 GPIO pins and whether common anode or cathode
 *    setState() -ON, OFF or BLINKING - blinking gives a short flash every 5 seconds (saves battery)
 *    setColour() - RED, AMBER, GREEN - traffic light colours
 */

 class TricolourLED {
  public:
      enum LED_COMMON( COMMON_ANODE, COMMON_CATHODE);
      enum LED_STATE( ON, OFF, BLINKING);
      enum LED_COLOUR( RED, GREEN, BLUE);
      TricolourLED(uint_8 red_pin, uint_8 green_pin, uint_8 blue_pin, LED_COMMON polarity );
      void setState(LED_STATE new_state);
      void setColour(LED_COLOUR new_colour);
  private:
 }