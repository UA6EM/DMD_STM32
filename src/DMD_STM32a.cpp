/*--------------------------------------------------------------------------------------

 DMD_STM32.cpp  - STM32 port of DMD.h library (see below)

 adapted by Dmitry Dmitriev (c) 2019

 DMD.cpp - Function and support library for the Freetronics DMD, a 512 LED matrix display
		   panel arranged in a 32 x 16 layout.

 Copyright (C) 2011 Marc Alexander (info <at> freetronics <dot> com)

 ---

 This program is free software: you can redistribute it and/or modify it under the terms
 of the version 3 GNU General Public License as published by the Free Software Foundation.

 This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License along with this program.
 If not, see <http://www.gnu.org/licenses/>.

--------------------------------------------------------------------------------------*/

#include "Arduino.h"
#include "DMD_STM32a.h"
/*--------------------------------------------------------------------------------------
 Setup and instantiation of DMD library
 Note this currently uses the SPI port for the fastest performance to the DMD, be
 careful of possible conflicts with other SPI port devices
--------------------------------------------------------------------------------------*/


DMD::DMD(DMD_Pinlist* _mux_pinlist, byte _pin_nOE, byte _pin_SCLK, byte panelsWide, byte panelsHigh,
	uint8_t n_Rows, DMD_Pinlist* _data_pinlist, bool d_buf, byte dmd_pixel_x, byte dmd_pixel_y)
	:Adafruit_GFX(panelsWide * dmd_pixel_x, panelsHigh * dmd_pixel_y), mux_cnt(_mux_pinlist->count), mux_pinlist(_mux_pinlist), nRows(n_Rows), 
	data_pinlist(_data_pinlist), pin_DMD_CLK(_data_pinlist->list[0]), pin_DMD_nOE(_pin_nOE), pin_DMD_SCLK(_pin_SCLK), DisplaysWide(panelsWide), DisplaysHigh(panelsHigh), dbuf(d_buf),
	DMD_PIXELS_ACROSS(dmd_pixel_x), DMD_PIXELS_DOWN(dmd_pixel_y)
{

	DisplaysTotal = DisplaysWide * DisplaysHigh;
	
	mux_pins = mux_pinlist->list;
	data_pins = &(data_pinlist->list[1]);
	data_pins_cnt = data_pinlist->count-1;
	
	// Look up port registers and pin masks ahead of time,
	// avoids many slow digitalWrite() calls later.
#if (defined(__STM32F1__) || defined(__STM32F4__))
	datasetreg = portSetRegister(pin_DMD_CLK);
	clk_clrmask = clkmask = digitalPinToBitMask(pin_DMD_CLK);
	latsetreg = portSetRegister(pin_DMD_SCLK);
	latmask = digitalPinToBitMask(pin_DMD_SCLK);
	oemask = digitalPinToBitMask(pin_DMD_nOE);
	oesetreg = portSetRegister(pin_DMD_nOE);
	muxsetreg = portSetRegister(mux_pins[0]);
	
#endif
	mux_mask2 = (uint32_t*)malloc((nRows + 1) * 4);
	
}
/*--------------------------------------------------------------------------------------*/
DMD::~DMD()
{
	free(mux_mask2);
	delete mux_pinlist;
	delete data_pinlist;
#if defined(DEBUG2)
	free((uint16_t*)dd_ptr);
#endif
}
/*--------------------------------------------------------------------------------------*/
void DMD::set_pin_modes() {
#if (defined(__STM32F1__) || defined(__STM32F4__))

	for (uint8_t i = 0; i < mux_cnt; i++) {
		digitalWrite(mux_pins[i], LOW);
		pinMode(mux_pins[i], OUTPUT);
	}
	digitalWrite(pin_DMD_CLK, LOW);
	pinMode(pin_DMD_CLK, OUTPUT);

	digitalWrite(pin_DMD_SCLK, LOW);
	pinMode(pin_DMD_SCLK, OUTPUT);
#if defined(__STM32F1__) 
	oe_channel = PIN_MAP[pin_DMD_nOE].timer_channel;
#elif defined(__STM32F4__) 
	oe_channel = timer_map[pin_DMD_nOE].channel;
#endif

	pinMode(pin_DMD_nOE, PWM);  // setup the pin as PWM
#endif
}

/*--------------------------------------------------------------------------------------*/
void DMD::init(uint16_t scan_interval) {
	
	this->set_pin_modes();

	// calculate update interval
	 scan_cycle_len = (uint32_t) scan_interval * CYCLES_PER_MICROSECOND;
   
   // here will be initialize_timers() call in child classes 


	 this->generate_muxmask();

    // clean both buffers
	if (matrixbuff[0] != matrixbuff[1]) {
		bDMDScreenRAM = matrixbuff[1 - backindex];
		clearScreen(true);
	}
	bDMDScreenRAM = matrixbuff[backindex];
	clearScreen(true);

	setBrightness(200);
	textcolor = 1;
	textbgcolor = 0;

#if defined(DEBUG2)
	if (!dd_ptr) dd_ptr = (uint16_t*)malloc(200);
#endif	
}
/*--------------------------------------------------------------------------------------*/
#if (defined(__STM32F1__) || defined(__STM32F4__))
void DMD::initialize_timers(voidFuncPtr handler) {

	if (handler != NULL) this->setup_main_timer(this->scan_cycle_len, handler);
	uint16 prescaler = timer_get_prescaler(MAIN_TIMER) + 1;
	timer_init(OE_TIMER);
	timer_pause(OE_TIMER);
	timer_set_prescaler(OE_TIMER, prescaler - 1);
	timer_oc_set_mode(OE_TIMER, oe_channel, (timer_oc_mode)this->OE_polarity, 0);
	timer_set_reload(OE_TIMER, TIM_MAX_RELOAD);
	timer_cc_enable(OE_TIMER, oe_channel);
	timer_generate_update(OE_TIMER);
	timer_resume(OE_TIMER);

}
#endif
#if (defined(ARDUINO_ARCH_RP2040))
/*--------------------------------------------------------------------------------------*/
void DMD::initialize_timers(voidFuncPtr handler) {

	
	this->scan_cycle_len = this->scan_cycle_len / this->pwm_clk_div;

	

	


	//pio configs
	sm_data = pio_claim_unused_sm(pio, true);
	//data_prog_offs = pio_add_program(pio, &dmd_out_program);
	data_prog_offs = pio_add_dmd_out_program(pio, this->data_pins_cnt);
	pio_config = dmd_out_program_init(pio, sm_data, data_prog_offs, pio_clkdiv, this->data_pins[0], this->data_pins_cnt, pin_DMD_SCLK, pin_DMD_CLK);
	

	//define timers numbers
	OE_slice_num = pwm_gpio_to_slice_num(pin_DMD_nOE);        // OE timer number from OE pin number
	if (OE_slice_num < 7) MAIN_slice_num = OE_slice_num + 1;  // set MAIN timer next to OE
	else MAIN_slice_num = 6;                                  // if OE timer is 7th - set MAIN to 6th

	// OE timer config
	pwm_config c_OE = pwm_get_default_config();
	pwm_config_set_clkdiv(&c_OE, pwm_clk_div);
	pwm_config_set_wrap(&c_OE, 0xFFEE);
	//pwm_config_set_output_polarity(&c_OE, false,false);   // non-invert A & B outputs (similar to PWM1 mode in STM32)
	pwm_config_set_output_polarity(&c_OE, this->OE_polarity, this->OE_polarity);
	gpio_set_function(pin_DMD_nOE, GPIO_FUNC_PWM);
	pwm_set_gpio_level(pin_DMD_nOE, this->scan_cycle_len / 2);

	// MAIN timer config
	pwm_config c_MAIN = pwm_get_default_config();
	pwm_config_set_clkdiv(&c_MAIN, pwm_clk_div);
	pwm_config_set_wrap(&c_MAIN, this->scan_cycle_len);
	pwm_clear_irq(MAIN_slice_num);
	pwm_set_irq_enabled(MAIN_slice_num, true);             // enable timer overflow irq
	irq_set_exclusive_handler(PWM_IRQ_WRAP, handler);
	irq_set_enabled(PWM_IRQ_WRAP, true);

	// DMA config
	dma_chan = dma_claim_unused_channel(true);
	dma_channel_config dma_c = dma_channel_get_default_config(dma_chan);
	channel_config_set_transfer_data_size(&dma_c, DMA_SIZE_8);     // read by one byte
	channel_config_set_read_increment(&dma_c, true);
	channel_config_set_dreq(&dma_c, sm_data - DREQ_PIO0_TX0);                 // requested by PIO

	dma_channel_configure(
		dma_chan,
		&dma_c,
		&pio0_hw->txf[sm_data], // Write address (only need to set this once)
		NULL,             // Don't provide a read address yet
	   	this->x_len,      // Write x_len bytes than stop
		false             // Don't start yet
	);

	pwm_init(MAIN_slice_num, &c_MAIN, true);         // start MAIN timer
	pwm_init(OE_slice_num, &c_OE, true);         // start OE timer

	
}
#endif
/*--------------------------------------------------------------------------------------*/
#if (defined(__STM32F1__) || defined(__STM32F4__))
uint16_t DMD::setup_main_timer(uint32_t cycles, voidFuncPtr handler) {

	timer_init(MAIN_TIMER);
	timer_pause(MAIN_TIMER);
	uint16 prescaler = (uint16)(cycles / TIM_MAX_RELOAD ) + 1;
	if (prescaler > 1) this->scan_cycle_len /= prescaler;
	
	timer_set_prescaler(MAIN_TIMER, prescaler - 1);
	timer_set_reload(MAIN_TIMER, this->scan_cycle_len);
	timer_attach_interrupt(MAIN_TIMER, TIMER_UPDATE_INTERRUPT, handler);
	timer_generate_update(MAIN_TIMER);
	timer_resume(MAIN_TIMER);
	return prescaler;

}
#endif
/*--------------------------------------------------------------------------------------*/
#if (defined(__STM32F1__) || defined(__STM32F4__))
void DMD::generate_muxmask() {

#define set_mux_ch_by_mask(x)  ((uint32_t) x)
#define clr_mux_ch_by_mask(x)  (((uint32_t)x) << 16)

	for (uint8_t i = 0; i < nRows; i++)
	{
		mux_mask2[i] = 0;
		if (mux_cnt == nRows)                // DIRECT MUX
		{
			for (uint8_t j = 0; j < nRows; j++)
			{
				uint16_t mux_ch_mask = digitalPinToBitMask(mux_pins[j]);
				// set selected channel to LOW, all other to HIGH
				if (i == j)
				{
					mux_mask2[i] |= clr_mux_ch_by_mask(mux_ch_mask);    //low
				}
				else
				{
					mux_mask2[i] |= set_mux_ch_by_mask(mux_ch_mask);    //high
				}
			}
		}
		else {                             // BINARY MUX
			for (uint8_t j = 0; (1 << j) < nRows; j++)
			{
				uint16_t mux_ch_mask = digitalPinToBitMask(mux_pins[j]);
				if (i & (1 << j))
				{
					mux_mask2[i] |= set_mux_ch_by_mask(mux_ch_mask);
				}
				else
				{
					mux_mask2[i] |= clr_mux_ch_by_mask(mux_ch_mask);
				}
			}
		}
	}
	mux_mask2[nRows] = mux_mask2[0];

}
/*--------------------------------------------------------------------------------------*/
#elif (defined(ARDUINO_ARCH_RP2040))
void DMD::generate_muxmask() {

	for (uint8_t i = 0; i < nRows; i++)
		{		
		if (mux_cnt == nRows)                // DIRECT MUX
			// set selected channel to LOW, all other to HIGH
			{
			mux_mask2[i] = ((1 << nRows) - 1) & (~(1 << i));
			}
		else {                             // BINARY MUX
			mux_mask2[i] = i;
			}
		}
	mux_mask2[nRows] = mux_mask2[0];

	sm_mux = pio_claim_unused_sm(pio, true);
	//uint8_t data_mux_offs = pio_add_program(pio, &dmd_mux_program);
	uint8_t data_mux_offs = pio_add_dmd_mux_program(pio, this->mux_cnt);
	dmd_mux_program_init(pio, sm_mux, data_mux_offs, this->mux_pins[0], this->mux_cnt);
}
#endif
/*--------------------------------------------------------------------------------------*/
void DMD::set_mux(uint8_t curr_row) {
#if (defined(__STM32F1__) || defined(__STM32F4__))
	*muxsetreg = mux_mask2[curr_row];
#elif (defined(ARDUINO_ARCH_RP2040))
    pio_sm_put_blocking(pio, sm_mux, mux_mask2[curr_row]);
#endif
}
/*--------------------------------------------------------------------------------------*/
void DMD::switch_row() {
#if (defined(__STM32F1__) || defined(__STM32F4__))

	// switch all LED OFF
	
	timer_pause(MAIN_TIMER);
	timer_pause(OE_TIMER);
	
	uint32_t overflow =  scan_cycle_len;
	uint32_t oe_duration = ((overflow * this->brightness) / 255);
	timer_set_compare(OE_TIMER, oe_channel, oe_duration);
#endif
	
	this->set_mux(bDMDByte);

	if (bDMDByte == 2) {
		if (swapflag == true) {    // Swap front/back buffers if requested
			backindex = 1 - backindex;
			swapflag = false;
			bDMDScreenRAM = matrixbuff[backindex]; // Back buffer
			front_buff = matrixbuff[1 - backindex]; // -> front buffer
		}
	}
	if (++bDMDByte > 3) bDMDByte = 0;

#if (defined(__STM32F1__) || defined(__STM32F4__))
	*latsetreg = latmask; // Latch data loaded during *prior* interrupt
	*latsetreg = latmask << 16;// Latch down
#if (CYCLES_PER_MICROSECOND > 100)
	delayMicroseconds(1);
#endif
	// reenable LEDs, restart timers
	timer_set_count(MAIN_TIMER, 0);
	timer_set_count(OE_TIMER, 0);
	timer_generate_update(MAIN_TIMER);
	timer_generate_update(OE_TIMER);
	timer_resume(OE_TIMER);
	timer_resume(MAIN_TIMER);

#endif
}
/*--------------------------------------------------------------------------------------*/
void DMD::transform_XY(int16_t& bX, int16_t& bY) {

	switch (rotation) {
	case 1:
		_swap_int16_t(bX, bY);
		bX = WIDTH - 1 - bX;
		break;
	case 2:
		bX = WIDTH - 1 - bX;
		bY = HEIGHT - 1 - bY;
		break;
	case 3:
		_swap_int16_t(bX, bY);
		bY = HEIGHT - 1 - bY;
		break;
	}

	byte row = bY / DMD_PIXELS_DOWN;

	if ((connectScheme == CONNECT_ZIGZAG) && (row % 2))
	{
		bX = (WIDTH - 1) - bX;

		bY = bY % DMD_PIXELS_DOWN;
		bY = (DMD_PIXELS_DOWN - 1) - bY;
		bY = row * DMD_PIXELS_DOWN + bY;

	}
}


/*--------------------------------------------------------------------------------------*/
// Drawing the text in the screen and prepare using it in the marquee (running text)
// note: only one marquee can be used at the time

void DMD::drawMarqueeX(const char* bChars, int left, int top, byte orientation)
{
	int len = 0;
	while (bChars[len] && len < MAX_STRING_LEN-1) { len++; }
	this->drawMarquee(bChars, len, left, top, orientation);
}
/*--------------------------------------------------------------------------------------*/
void DMD::drawMarquee(const char* bChars, int length, int left, int top, byte orientation)
{
	
	// temp parameter for beta version
	uint8_t matrix_h = 16;

	// exit if string is empty of length == 0 
	if ((!bChars) || (!length)) return;
	
	if (length > MAX_STRING_LEN - 1) length = MAX_STRING_LEN - 1;
	
	strncpy(marqueeText, bChars, length);
	marqueeText[length] = '\0';

	stringBounds(marqueeText, length, &marqueeWidth, &marqueeMarginH, &marqueeMarginL, orientation);
	
	if (orientation == 1) {
		marqueeHeight = matrix_h;
	}
	else {
		marqueeHeight = Font->get_height();
	}

	marqueeOffsetY = top;
	marqueeOffsetX = left;
	marqueeLength = length;
	this->drawMarqueeString(marqueeOffsetX, marqueeOffsetY, marqueeText, marqueeLength,
		marqueeMarginH, marqueeMarginL, orientation);
}
/*--------------------------------------------------------------------------------------
 Service routine to call drawString<color16> instance inside the marquee methods.
               Virtual, have to override in DMD_RGB class.
 --------------------------------------------------------------------------------------*/
void  DMD::drawMarqueeString(int bX, int bY, const char* bChars, int length,
	int16_t miny, int16_t maxy, byte orientation)
	{
	this->drawString(bX, bY, bChars, length, textcolor, miny, maxy, orientation);
	}

/*--------------------------------------------------------------------------------------*/
// Moving marquee, prepared by drawMarquee() method, by one step. 
// return values:
//    MARQUEE_OUT_OF_SCREEN			- text has left the screen
//    MARQUEE_JUSTIFY_ON_LEFT 		- leftmost pixel of the text is on screen position 0
//    MARQUEE_JUSTIFY_ON_RIGHT		- rightmost pixel of the text is on the right end of screen
//        or returns 0 in any other case

uint8_t DMD::stepMarquee(int amountX, int amountY, byte orientation)
{
	uint8_t ret = 0;
	int16_t old_x = marqueeOffsetX;
	int16_t old_y = marqueeOffsetY;
	marqueeOffsetX += amountX;
	marqueeOffsetY += amountY;
	

	// check if marquee reached to the limits of matrix panel
	// X axis
	if (marqueeOffsetX < -marqueeWidth) {
		marqueeOffsetX = _width;
		ret |= 1;
	}
	else if (marqueeOffsetX > _width) {
		marqueeOffsetX = -marqueeWidth;
		ret |= 1;
	}
	// Y axis
	if (marqueeOffsetY < -marqueeHeight) {
		marqueeOffsetY = _height;
		ret |= 1;
	}
	else if (marqueeOffsetY > _height) {
		marqueeOffsetY = -marqueeHeight;
		ret |= 1;
	}
	if (ret) {
		//clearScreen(true);
		fillScreen(textbgcolor);
	}
	// if text left justify at screen
	if (marqueeOffsetX == 0) {
		ret |= 2;
	}
	// if text right justify at screen
	if ((marqueeOffsetX + marqueeWidth) == DMD_PIXELS_ACROSS * DisplaysWide) {
		ret |= 4;
	}
	// Special case horizontal scrolling to improve speed
	if (amountY == 0 && use_shift &&
		((amountX == -1) || (amountX == 1))) {
		// Shift entire screen one pixel
		shiftScreen(amountX);

		uint16_t limit_X = 0;                 // if (amountX == 1)
		if (amountX == -1) {  // if (amountX == -1)
			limit_X = _width;
			if (marqueeOffsetX < (_width - marqueeWidth)) return ret;
		}
		else {
			if (marqueeOffsetX > 0) return ret;
		}
		// Redraw last char on screen
		int strWidth = marqueeOffsetX;
		for (int i = 0; i < marqueeLength; i++) {
			int wide = charWidth(marqueeText[i], orientation);
			if (wide > 0) {
				if (strWidth + wide >= limit_X) {
					uint16_t curr_color = get_marquee_text_color(i);
					this->drawChar(strWidth, marqueeOffsetY, marqueeText[i], curr_color, marqueeMarginH, marqueeMarginL, orientation);
					return ret;
				}
				strWidth += wide + 1;
			}
		}

	}
	else {

		if (amountY > 0)	drawFilledBox(marqueeOffsetX, old_y + marqueeMarginH,
			marqueeOffsetX + marqueeWidth, marqueeOffsetY + marqueeMarginH,
			textbgcolor);

		else if (amountY < 0) drawFilledBox(marqueeOffsetX, marqueeOffsetY + marqueeMarginL,
				marqueeOffsetX + marqueeWidth, old_y + marqueeMarginL, textbgcolor);

		else if (amountX > 0) drawFilledBox(old_x, marqueeOffsetY + marqueeMarginH,
			marqueeOffsetX, marqueeOffsetY + marqueeMarginL, textbgcolor);

		else if (amountX < 0) drawFilledBox(marqueeOffsetX + marqueeWidth, marqueeOffsetY + marqueeMarginH,
			old_x + marqueeWidth, marqueeOffsetY + marqueeMarginL,
			textbgcolor);

      
		this->drawMarqueeString(marqueeOffsetX, marqueeOffsetY, marqueeText, marqueeLength,
			marqueeMarginH, marqueeMarginL, orientation);
	}

	return ret;
}

/*--------------------------------------------------------------------------------------
 Clear the screen in DMD RAM
--------------------------------------------------------------------------------------*/
void DMD::fillScreen(uint16_t color)
{
	this->clearScreen(inverse_color(color));
}
/*--------------------------------------------------------------------------------------*/
void DMD::clearScreen(byte bNormal)
{
	if (bNormal ^ inverse_ALL_flag) // clear all pixels
		memset(bDMDScreenRAM, 0xFF, mem_Buffer_Size);
	else // set all pixels
		memset(bDMDScreenRAM, 0x00, mem_Buffer_Size);
}

/*--------------------------------------------------------------------------------------
 Draw or clear a filled box(rectangle) with a single pixel border
--------------------------------------------------------------------------------------*/
void DMD::drawFilledBox(int x1, int y1, int x2, int y2,
	uint16_t color)
{
	if ((x2 < x1) || (y2 < y1)) return;

	for (int b = y1; b <= y2; b++) {

		//if (use_FastHLine) drawFastHLine(x1, b, (x2 - x1), color);
		//else writeLine(x1, b, x2, b, color);
		drawFastHLine(x1, b, (x2 - x1) + 1, color);
	}
}

/*--------------------------------------------------------------------------------------
	   Select current font
--------------------------------------------------------------------------------------*/
void DMD::selectFont(DMD_Font* font)
{
	this->Font = font;
}

/*--------------------------------------------------------------------------------------
  draw char with selected font at coordinates bX bY
--------------------------------------------------------------------------------------*/
int DMD::drawChar(const int bX, const int bY, const unsigned char letter, uint16_t color, byte orientation)
{
	uint8_t height = Font->get_height();
	return drawChar(bX, bY, letter, color, 0, height, orientation);
}

/*--------------------------------------------------------------------------------------*/
int DMD::drawChar(const int bX, const int bY, const unsigned char letter, uint16_t color, int16_t miny, int16_t maxy, byte orientation)
{
	
	if ((bX >= _width) || (bY >= _height)) return -1;

	unsigned char c = letter;
	if (!Font->is_char_in(c)) return 0;

	uint8_t fg_col_bytes[4];
	uint8_t bg_col_bytes[4];
	uint16_t bg_color = inverse_color(color);
	uint8_t height = Font->get_height();
	// temp parameter for beta version
	uint8_t matrix_h = 16;
	
	if (c == ' ') { //CHANGED FROM ' '
		if (orientation) { // vertical scroll
			this->drawFilledBox(bX, bY, bX + height, bY + matrix_h, bg_color);
			return height;
		}
		else {   // normal scroll
			int charWide = Font->get_char_width(' ');
			this->drawFilledBox(bX, bY + miny, bX + charWide, bY + maxy, bg_color);
			return charWide;
		}
	}

	if (Font->is_gfx_font()) {

		DMD_GFX_Font* ff = (DMD_GFX_Font*)Font;
		GFXfont* gfxFont_p = ff->get_font_by_char(c);
		c -= ff->get_first_by_char(c);

#if (defined(__STM32F1__) || defined(__STM32F4__))
		GFXglyph* glyph = &((gfxFont_p->glyph)[c]);
		uint8_t* bitmap = gfxFont_p->bitmap;
#else
		GFXglyph* glyph = &(((GFXglyph*)pgm_read_pointer(&gfxFont_p->glyph))[c]);
		uint8_t* bitmap = (uint8_t*)pgm_read_pointer(&gfxFont_p->bitmap);
#endif		
	
		uint16_t bo = pgm_read_word(&glyph->bitmapOffset);
		uint8_t  w = pgm_read_byte(&glyph->width);
		uint8_t	 h = pgm_read_byte(&glyph->height);
		int8_t  yo = height + (int8_t)pgm_read_byte(&glyph->yOffset);
		uint8_t  xx, yy, bits = 0, bit = 0, bsize, hh;
		int8_t   xo = (int8_t)pgm_read_byte(&glyph->xOffset);
		uint8_t  ww = pgm_read_byte(&glyph->xAdvance);
		if (orientation) {   // vertical scroll
			xo = yo;
			yo = w + (matrix_h - w) / 2;
			hh = xo + h;
			ww = hh;
			this->drawFilledBox(bX, bY, bX + hh, bY + matrix_h, bg_color);
		}
		else {
			if (fast_Hbyte) {
				getColorBytes(fg_col_bytes, color);
				getColorBytes(bg_col_bytes, bg_color);
			}
			this->drawFilledBox(bX, bY + miny, bX + ww, bY + maxy, bg_color);
		}



		for (yy = 0; yy < h; yy++) {
			for (xx = 0; xx < w; xx++) {
				if (!(bit++ & 7)) {
					bits = pgm_read_byte(&bitmap[bo++]);
				}

				if ((!orientation) && (fast_Hbyte)) {
					bsize = ((w - xx) > 8) ? 8 : (w - xx);
					uint8_t bbit = (bit - 1) & 7;
					if (bsize > (8 - bbit)) bsize = 8 - bbit;
					drawHByte(bX + xo + xx, bY + yo + yy, bits, bsize, fg_col_bytes, bg_col_bytes);
					bit += bsize - 1;
					xx += bsize - 1;
					bits <<= bsize;
				}
				else {
					uint16_t col;
					if (bits & 0x80) { col = color; }
					else { col = bg_color; }
					if (orientation) { // vertical scroll
						writePixel(bX + xo + yy, bY + yo - xx, col);
					}
					else {
						writePixel(bX + xo + xx, bY + yo + yy, col);
					}
					bits <<= 1;
				}

			}
		}

		return ww;
	}
	else {


		DMD_Standard_Font* ff = (DMD_Standard_Font*)Font;
		uint8_t width = ff->get_char_width(c);
		uint8_t bytes = (height + 7) / 8;
		uint16_t index = ff->get_bitmap_index(c);
		c -= ff->get_first();

		if (bX < -width || bY < -height) return width;



		// last but not least, draw the character
		for (uint8_t j = 0; j < width; j++) { // Width
			for (uint8_t i = bytes - 1; i < 254; i--) { // Vertical Bytes
				uint8_t data = pgm_read_byte(ff->font_ptr + index + j + (i * width));
				int offset = (i * 8);
				if ((i == bytes - 1) && bytes > 1) {
					offset = height - 8;
				}
				for (uint8_t k = 0; k < 8; k++) { // Vertical bits
					if ((offset + k >= i * 8) && (offset + k <= height)) {
						if (data & (1 << k)) {
							writePixel(bX + j, bY + offset + k, color);
						}
						else {
							writePixel(bX + j, bY + offset + k, bg_color);
						}
					}
				}
			}
		}
		return width;
	}
}

/*--------------------------------------------------------------------------------------
		  char width in pixels with selected font
			 routine moved to DMD_Font classes
--------------------------------------------------------------------------------------*/

int DMD::charWidth(const unsigned char letter, byte orientation)
{
	return (uint8_t)Font->get_char_width(letter, orientation);
}

/*--------------------------------------------------------------------------------------
      String bounds in pixels with selected font.
	  Set values of string width as w, 
	  and it upmost and lowest points y-coordinates (min_y max_y)
	  relative to base point (upper left corner) of the text.
--------------------------------------------------------------------------------------*/
void DMD::stringBounds(const char* bChars, uint16_t length,
	int16_t* w, int16_t* min_y, int16_t* max_y, byte orientation) {

	uint8_t height = Font->get_height();

	if (length == 0) {
		while (bChars[length] && length < MAX_STRING_LEN-1) { length++; }
	}

	if (Font->is_gfx_font()) {

		DMD_GFX_Font* ff = (DMD_GFX_Font*)Font;
		uint16_t width = 0;
		int16_t minx = _width, miny = _height, maxx = -1, maxy = -1;
		for (int i = 0; i < length; i++) {

			char c = bChars[i];

			if (ff->is_char_in(c)) {

				gfxFont = ff->get_font_by_char(c);
				if (gfxFont == ff->gfxFont2) {
					c += pgm_read_byte(&gfxFont->first) - ff->get_first_by_char(c);
				}
				int16_t x = 0, y = 0;
				if (orientation) {
					miny = _height; maxy = -1;
					charBounds(c, &x, &y, &minx, &miny, &maxx, &maxy);
					width += height + maxy + 2;
				}
				else {
					charBounds(c, &x, &y, &minx, &miny, &maxx, &maxy);
					if (x > 0) width += x + 1;
				}
			}
		}
		if (width) width--;
		*w = width;
		if (orientation) {
			*min_y = 0;
			*max_y = height;
		}
		else {
			*min_y = miny + height;
			*max_y = maxy + height + 1;
		}
	}
	else {
		*w = stringWidth(bChars, length, orientation);
		*min_y = 0;
		*max_y = height - 1;
	}
}

/*--------------------------------------------------------------------------------------
   string width in pixels
--------------------------------------------------------------------------------------*/
uint16_t DMD::stringWidth(const char* bChars, uint16_t length, byte orientation)
{
	// this->Font
	uint16_t width = 0;
	if (length == 0) {

		while (bChars[length] && length < MAX_STRING_LEN-1) { length++; }
	}
	// char c;
	int idx;

	for (idx = 0; idx < length; idx++) {
		int cwidth = charWidth(bChars[idx], orientation);
		if (cwidth > 0)
			width += cwidth + 1;
	}
	if (width) {
		width--;
	}
	return width;

}

#if defined(DEBUG2)
void DMD::dumpMatrix(void) {

	int i, buffsize = mem_Buffer_Size;

	Serial.print(F("\n\n"
		"#include <avr/pgmspace.h>\n\n"
		"static const uint8_t PROGMEM img[] = {\n  "));

	for (i = 0; i < buffsize; i++) {
		Serial.print(F("0x"));
		if (matrixbuff[backindex][i] < 0x10) Serial.write('0');
		Serial.print(matrixbuff[backindex][i], HEX);
		if (i < (buffsize - 1)) {
			if ((i & 7) == 7) Serial.print(F(",\n  "));
			else             Serial.write(',');
		}
	}
	Serial.println(F("\n};"));
}
void DMD::dumpDDbuf(void) {

	uint16_t i, buffsize = 100;

	Serial.begin(115200);
	Serial.print("Prescaler: ");
	Serial.println(timer_get_prescaler(OE_TIMER));

	for (i = 0; i < buffsize; i++) {


		Serial.print(dd_ptr[i]);
		if (i < (buffsize - 1)) {
			if ((i & 7) == 7) Serial.print(F(",\n  "));
			else             Serial.write(',');
		}
	}
	Serial.println(F("\n};"));
}

#endif	
/*--------------------------------------------------------------------------------------*/
void DMD::swapBuffers(boolean copy) {
	if (matrixbuff[0] != matrixbuff[1]) {
		// To avoid 'tearing' display, actual swap takes place in the interrupt
		// handler, at the end of a complete screen refresh cycle.
		swapflag = true;                  // Set flag here, then...
		while (swapflag == true) delay(1); // wait for interrupt to clear it
		bDMDScreenRAM = matrixbuff[backindex]; // Back buffer
		front_buff = matrixbuff[1 - backindex]; // -> front buffer
		if (copy == true)
			memcpy(matrixbuff[backindex], matrixbuff[1 - backindex], mem_Buffer_Size);
	}
}
