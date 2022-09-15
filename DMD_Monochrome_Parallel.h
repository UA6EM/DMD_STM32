#pragma once
#if (defined(__STM32F1__) || defined(__STM32F4__))
#include "DMD_STM32a.h"
class DMD_Monochrome_Parallel :
	public DMD
{
public:
	DMD_Monochrome_Parallel(byte _pin_A, byte _pin_B, byte _pin_nOE, byte _pin_SCLK, uint8_t* pinlist,
		byte panelsWide, byte panelsHigh, bool d_buf = false, byte dmd_pixel_x = 32, byte dmd_pixel_y = 16);

	~DMD_Monochrome_Parallel();

	void init(uint16_t scan_interval = 2000) override;
	void set_pin_modes() override;
	void drawPixel(int16_t x, int16_t y, uint16_t color) override;
	void scan_dmd();
	void clearScreen(byte bNormal)  override;
	void shiftScreen(int8_t step)  override;
	// changing connect scheme not allowed for Parallel
	virtual void setConnectScheme(uint8_t sch) {};

private:

	uint16_t mux_size, column_size;

	// Pin bitmasks
	PortType clk_clrmask, clkmask;
#ifdef USE_UPPER_8BIT
	PortType clk_clrmask_low, clkmask_low;
#endif // USE_UPPER_8BIT
	byte row_pins[8];
	PortType row_mask[8];
	// PORT register pointers 
	volatile PortType* datasetreg; 

};
#endif

