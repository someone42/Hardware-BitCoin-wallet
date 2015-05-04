/** \file lcd_and_input.c
  *
  * \brief HD44780-based LCD driver and input button reader.
  *
  * It's assumed that the LCD has 2 lines, each character is 5x8 dots and
  * there are 40 bytes per line of DDRAM.
  * The datasheet was obtained on 22-September-2011, from:
  * http://lcd-linux.sourceforge.net/pdfdocs/hd44780.pdf
  *
  * All references to "the datasheet" refer to this document.
  *
  * This also (incidentally) deals with button inputs, since there's a
  * timer ISR which can handle the debouncing. The pin assignments in this
  * file are referred to by their Arduino pin mapping; if not using an
  * Arduino, see http://arduino.cc/en/Hacking/PinMapping168
  * for pin mappings.
  *
  * This file is licensed as described by the file LICENCE.
  */

#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <string.h>

#include "../common.h"
#include "../hwinterface.h"
#include "../baseconv.h"
#include "../prandom.h"

/** Maximum number of address/amount pairs that can be stored in RAM waiting
  * for approval from the user. This incidentally sets the maximum
  * number of outputs per transaction that parseTransaction() can deal with.
  * \warning This must be < 256.
  */
#define MAX_OUTPUTS		2

/**
 * \defgroup LCDPins Arduino pin numbers that the LCD is connected to.
 *
 * @{
 */
/** Register select. */
#define RS_PIN			12
/** Begin read/write. */
#define E_PIN			11
/** First (least significant bit) data pin. */
#define D4_PIN			5
/** Second data pin. */
#define D5_PIN			4
/** Third data pin. */
#define D6_PIN			3
/** Fourth (most significant bit) data pin. */
#define D7_PIN			2
/**@}*/

/** The Arduino pin number that the accept button is connected to. */
#define ACCEPT_PIN		6
/** The Arduino pin number that the cancel button is connected to. */
#define	CANCEL_PIN		7

/** Number of columns per line. */
#define NUM_COLUMNS		16
/** Scroll speed, in multiples of 5 ms. Example: 100 means scroll will happen
  * every 500 ms.
  * \warning This must be < 65536.
  */
#define SCROLL_SPEED	150
/** Scroll pause length, in multiples of 5 ms. Whenever a string is written
  * to the LCD, the display will pause for this long before scrolling starts.
  * direction.
  * \warning This must be < 65536.
  */
#define SCROLL_PAUSE	450

/** Number of consistent samples (each sample is 5 ms apart) required to
  * register a button press.
  * \warning This must be < 256.
  */
#define DEBOUNCE_COUNT	8

/** Set one of the digital output pins based on the Arduino pin mapping.
  * \param pin The Arduino pin number (0 to 13 inclusive) to set.
  * \param value Non-zero will set the pint high and zero will set the pin
  *              low.
  */
static inline void writeArduinoPin(const uint8_t pin, const uint8_t value)
{
	uint8_t bit;

	bit = 1;
	if (pin < 8)
	{
		bit = (uint8_t)(bit << pin);
		DDRD |= bit;
		if (value)
		{
			PORTD |= bit;
		}
		else
		{
			PORTD = (uint8_t)(PORTD & ~bit);
		}
	}
	else
	{
		bit = (uint8_t)(bit << (pin - 8));
		DDRB |= bit;
		if (value)
		{
			PORTB |= bit;
		}
		else
		{
			PORTB = (uint8_t)(PORTB & ~bit);
		}
	}
}

/** Write the least-significant 4 bits of value to the HD44780.
  * See page 49 of the datasheet for EN timing. All delays have at least
  * a 2x safety factor.
  * \param value The 4 bit value to write.
  */
static void write4(uint8_t value)
{
	writeArduinoPin(D4_PIN, (uint8_t)(value & 0x01));
	writeArduinoPin(D5_PIN, (uint8_t)(value & 0x02));
	writeArduinoPin(D6_PIN, (uint8_t)(value & 0x04));
	writeArduinoPin(D7_PIN, (uint8_t)(value & 0x08));
	_delay_us(2);
	writeArduinoPin(E_PIN, 0);
	_delay_us(2);
	writeArduinoPin(E_PIN, 1);
	_delay_us(2);
	writeArduinoPin(E_PIN, 0);
	_delay_us(2);
	// From page 24 of the datasheet, most commands require 37 us to complete.
	_delay_us(74);
}

/** Write 8 bits to the HD44780 using write4() twice.
  * \param value The 8 bit value to write.
  * \warning Make sure register select (#RS_PIN) is set to an appropriate
  *          value before calling this function.
  */
static void write8(uint8_t value)
{
	write4((uint8_t)(value >> 4));
	write4(value);
}

/** Set one of the Arduino digital I/O pins to be an input pin with
  * internal pull-up enabled.
  * \param pin The Arduino pin number to set as an input pin.
  */
static inline void setArduinoPinInput(const uint8_t pin)
{
	uint8_t bit;

	bit = 1;
	if (pin < 8)
	{
		bit = (uint8_t)(bit << pin);
		DDRD = (uint8_t)(DDRD & ~bit);
		PORTD |= bit;
	}
	else
	{
		bit = (uint8_t)(bit << (pin - 8));
		DDRB = (uint8_t)(DDRB & ~bit);
		PORTB |= bit;
	}
}

/** Read one of the Arduino digital I/O pins.
  * \param pin The Arduino pin number to read.
  * \return Non-zero if the pin is high, 0 if it is low.
  */
static inline uint8_t sampleArduinoPin(const uint8_t pin)
{
	uint8_t bit;

	bit = 1;
	if (pin < 8)
	{
		bit = (uint8_t)(bit << pin);
		return (uint8_t)(PIND & bit);
	}
	else
	{
		bit = (uint8_t)(bit << (pin - 8));
		return (uint8_t)(PINB & bit);
	}
}

/** 0-based column index. This specifies which column on the LCD the next
  * character will appear in. */
static uint8_t current_column;
/** Largest size (in number of characters) of either line. */
static uint8_t max_line_size;
/** Scroll position (0 = leftmost) in number of characters. */
static uint8_t scroll_pos;
/** false = towards the right (text appears to move left), true = towards
  * the left (text appears to move right). */
static uint8_t scroll_to_left;
/** Countdown to next scroll. */
static uint16_t scroll_counter;
/** Status of accept button; false = not pressed, true = pressed. */
static volatile bool accept_button;
/** Status of cancel button; false = not pressed, true = pressed. */
static volatile bool cancel_button;
/** Debounce counter for accept button. */
static uint8_t accept_debounce;
/** Debounce counter for cancel button. */
static uint8_t cancel_debounce;

/** Storage for the text of transaction output amounts. */
static char list_amount[MAX_OUTPUTS][TEXT_AMOUNT_LENGTH];
/** Storage for the text of transaction output addresses. */
static char list_address[MAX_OUTPUTS][TEXT_ADDRESS_LENGTH];
/** Index into #list_amount and #list_address which specifies where the next
  * output amount/address will be copied into. */
static uint8_t list_index;
/** Whether the transaction fee has been set. If
  * the transaction fee still hasn't been set after parsing, then the
  * transaction is free. */
static bool transaction_fee_set;
/** Storage for transaction fee amount. This is only valid
  * if #transaction_fee_set is true. */
static char transaction_fee_amount[TEXT_AMOUNT_LENGTH];

/** This does the scrolling and checks the state of the buttons. */
ISR(TIMER0_COMPA_vect)
{
	bool temp;

	scroll_counter--;
	if (scroll_counter == 0)
	{
		if (max_line_size > NUM_COLUMNS)
		{
			if (scroll_to_left)
			{
				if (scroll_pos == 0)
				{
					scroll_to_left = false;
				}
				else
				{
					writeArduinoPin(RS_PIN, 0);
					write8(0x1c);
					scroll_pos--;
				}
			}
			else
			{
				if (scroll_pos == (max_line_size - NUM_COLUMNS))
				{
					scroll_to_left = true;
				}
				else
				{
					writeArduinoPin(RS_PIN, 0);
					write8(0x18);
					scroll_pos++;
				}
			}
		}
		scroll_counter = SCROLL_SPEED;
	}

	if (sampleArduinoPin(ACCEPT_PIN) != 0)
	{
		temp = true;
	}
	else
	{
		temp = false;
	}
	if ((accept_button && temp) || (!accept_button && !temp))
	{
		// Mismatching state; accumulate debounce counter until threshold
		// is reached, then make states consistent.
		accept_debounce++;
		if (accept_debounce == DEBOUNCE_COUNT)
		{
			accept_button = !accept_button;
		}
	}
	else
	{
		accept_debounce = 0;
	}
	temp = sampleArduinoPin(CANCEL_PIN);
	if ((cancel_button && temp) || (!cancel_button && !temp))
	{
		// Mismatching state; accumulate debounce counter until threshold
		// is reached, then make states consistent.
		cancel_debounce++;
		if (cancel_debounce == DEBOUNCE_COUNT)
		{
			cancel_button = !cancel_button;
		}
	}
	else
	{
		cancel_debounce = 0;
	}
}

/** Clear LCD of all text. */
static void clearLcd(void)
{
	current_column = 0;
	max_line_size = 0;
	scroll_pos = 0;
	scroll_to_left = false;
	scroll_counter = SCROLL_SPEED;
	writeArduinoPin(RS_PIN, 0);
	write8(0x01); // clear display
	_delay_ms(10);
}

/** See page 46 of the datasheet for the HD44780 initialisation sequence. All
  * delays have a 2x safety factor. This also sets up timer 0 to fire an
  * interrupt every 5 ms.
  */
void initLcdAndInput(void)
{
	cli();
	TCCR0A = _BV(WGM01); // CTC mode
	TCCR0B = _BV(CS02) | _BV(CS00); // prescaler 1024
	TCNT0 = 0;
	OCR0A = 77; // frequency = (16000000 / 1024) / (77 + 1) = 200 Hz
	TIMSK0 = _BV(OCIE0A); // enable interrupt on compare match A
	scroll_counter = 1000; // make sure no attempt at scrolling is made yet
	MCUCR = (uint8_t)(MCUCR & ~_BV(PUD));
	setArduinoPinInput(ACCEPT_PIN);
	setArduinoPinInput(CANCEL_PIN);
	accept_button = false;
	cancel_button = false;
	accept_debounce = 0;
	cancel_debounce = 0;
	sei();
	writeArduinoPin(E_PIN, 0);
	writeArduinoPin(RS_PIN, 0);
	_delay_ms(80);
	write4(3);
	_delay_ms(8.2);
	write4(3);
	_delay_ms(0.2);
	write4(3);
	write4(2);
	// Now in 4 bit mode.
	write8(0x28); // function set: 4 bit mode, 2 lines, 5x8 dots
	write8(0x0c); // display on/off control: display on, no cursor
	clearLcd();
	write8(0x06); // entry mode set: increment, no display shift
	list_index = 0;
}

/** Set LCD cursor position to the start of a line.
  * \param line If this is zero, the cursor will be set to the start of the
  *             first line, otherwise the cursor will be set to the start of
  *             the second line.
  */
static void gotoStartOfLine(uint8_t line)
{
	writeArduinoPin(RS_PIN, 0);
	if (line == 0)
	{
		write8(0x80);
	}
	else
	{
		write8(0xc0);
	}
	current_column = 0;
}

/** Write a null-terminated string to the display.
  * \param str The null-terminated string to write.
  * \param is_progmem If this is true, then str is treated as a pointer
  *                   to program memory (data with the #PROGMEM attribute),
  *                   otherwise str is treated as a pointer to RAM.
  * \warning Characters past column 40 are dropped (ignored).
  */
static void writeString(const char *str, bool is_progmem)
{
	char c;

	writeArduinoPin(RS_PIN, 1);
	if (is_progmem)
	{
		c = (char)pgm_read_byte(str);
	}
	else
	{
		c = *str;
	}
	str++;
	while ((c != 0) && (current_column < 40))
	{
		write8((uint8_t)c);
		if (is_progmem)
		{
			c = (char)pgm_read_byte(str);
		}
		else
		{
			c = *str;
		}
		str++;
		current_column++;
		if (current_column > max_line_size)
		{
			max_line_size = current_column;
		}
	}
	scroll_counter = SCROLL_PAUSE;
}

/** Notify the user interface that the transaction parser has seen a new
  * Bitcoin amount/address pair.
  * \param text_amount The output amount, as a null-terminated text string
  *                    such as "0.01".
  * \param text_address The output address, as a null-terminated text string
  *                     such as "1RaTTuSEN7jJUDiW1EGogHwtek7g9BiEn".
  * \return false if no error occurred, true if there was not enough space to
  *         store the amount/address pair.
  */
bool newOutputSeen(char *text_amount, char *text_address)
{
	char *amount_dest;
	char *address_dest;

	if (list_index >= MAX_OUTPUTS)
	{
		return true; // not enough space to store the amount/address pair
	}
	amount_dest = list_amount[list_index];
	address_dest = list_address[list_index];
	strncpy(amount_dest, text_amount, TEXT_AMOUNT_LENGTH);
	strncpy(address_dest, text_address, TEXT_ADDRESS_LENGTH);
	amount_dest[TEXT_AMOUNT_LENGTH - 1] = '\0';
	address_dest[TEXT_ADDRESS_LENGTH - 1] = '\0';
	list_index++;
	return false; // success
}

/** Notify the user interface that the transaction parser has seen the
  * transaction fee. If there is no transaction fee, the transaction parser
  * will not call this.
  * \param text_amount The transaction fee, as a null-terminated text string
  *                    such as "0.01".
  */
void setTransactionFee(char *text_amount)
{
	strncpy(transaction_fee_amount, text_amount, TEXT_AMOUNT_LENGTH);
	transaction_fee_amount[TEXT_AMOUNT_LENGTH - 1] = '\0';
	transaction_fee_set = true;
}

/** Notify the user interface that the list of Bitcoin amount/address pairs
  * should be cleared. */
void clearOutputsSeen(void)
{
	list_index = 0;
	transaction_fee_set = false;
}

/** Wait until neither accept nor cancel buttons are being pressed. */
static void waitForNoButtonPress(void)
{
	do
	{
		// do nothing
	} while (accept_button || cancel_button);
}

/** Wait until accept or cancel button is pressed.
  * \return false if the accept button was pressed, true if the cancel
  *         button was pressed.
  */
static bool waitForButtonPress(void)
{
	bool current_accept_button;
	bool current_cancel_button;

	do
	{
		// Copy to avoid race condition.
		current_accept_button = accept_button;
		current_cancel_button = cancel_button;
	} while (!current_accept_button && !current_cancel_button);
	if (current_accept_button)
	{
		return false;
	}
	else
	{
		return true;
	}
}

/**
 * \defgroup AskStrings String literals for user prompts.
 *
 * The code would be much more readable if the string literals were all
 * implicitly defined within userDenied(). However, then they eat up valuable
 * RAM. Declaring them here means that they can have the #PROGMEM attribute
 * added (to place them in program memory).
 *
 * @{
 */
/** First line of #ASKUSER_NUKE_WALLET prompt. */
static const char str_delete_line0[] PROGMEM = "Delete existing wallet";
/** Second line of #ASKUSER_NUKE_WALLET prompt. */
static const char str_delete_line1[] PROGMEM = "and start a new one?";
/** First line of #ASKUSER_NEW_ADDRESS prompt. */
static const char str_new_line0[] PROGMEM = "Create new";
/** Second line of #ASKUSER_NEW_ADDRESS prompt. */
static const char str_new_line1[] PROGMEM = "address?";
/** What will be prepended to output amounts for #ASKUSER_SIGN_TRANSACTION
  * prompt. */
static const char str_sign_part0[] PROGMEM = "Sending ";
/** What will be appended to output amounts for #ASKUSER_SIGN_TRANSACTION
  * prompt. */
static const char str_sign_part1[] PROGMEM = " BTC to";
/** What will be prepended to the transaction fee amount
  * for #ASKUSER_SIGN_TRANSACTION prompt. */
static const char str_fee_part0[] PROGMEM = "Transaction fee:";
/** What will be appended to the transaction fee amount
  * for #ASKUSER_SIGN_TRANSACTION prompt. */
static const char str_fee_part1[] PROGMEM = " BTC";
/** First line of #ASKUSER_FORMAT prompt. */
static const char str_format_line0[] PROGMEM = "Do you want to";
/** Second line of #ASKUSER_FORMAT prompt. */
static const char str_format_line1[] PROGMEM = "delete everything?";
/** First line of #ASKUSER_CHANGE_NAME prompt. */
static const char str_change_name_line0[] PROGMEM = "Change the name";
/** Second line of #ASKUSER_CHANGE_NAME prompt. */
static const char str_change_name_line1[] PROGMEM = "of your wallet?";
/** First line of #ASKUSER_BACKUP_WALLET prompt. */
static const char str_backup_line0[] PROGMEM = "Do you want to do";
/** Second line of #ASKUSER_BACKUP_WALLET prompt. */
static const char str_backup_line1[] PROGMEM = "a wallet backup?";
/** First line of #ASKUSER_RESTORE_WALLET prompt. */
static const char str_restore_line0[] PROGMEM = "Restore wallet";
/** Second line of #ASKUSER_RESTORE_WALLET prompt. */
static const char str_restore_line1[] PROGMEM = "from backup?";
/** First line of #ASKUSER_CHANGE_KEY prompt. */
static const char str_change_key_line0[] PROGMEM = "Change the key";
/** Second line of #ASKUSER_CHANGE_KEY prompt. */
static const char str_change_key_line1[] PROGMEM = "of your wallet?";
/** First line of #ASKUSER_GET_MASTER_KEY prompt. */
static const char str_get_master_key_line0[] PROGMEM = "Reveal master";
/** Second line of #ASKUSER_GET_MASTER_KEY prompt. */
static const char str_get_master_key_line1[] PROGMEM = "public key?";
/** First line of unknown prompt. */
static const char str_unknown_line0[] PROGMEM = "Unknown command in userDenied()";
/** Second line of unknown prompt. */
static const char str_unknown_line1[] PROGMEM = "Press any button to continue";
/** What will be displayed if a stream read or write error occurs. */
static const char str_stream_error[] PROGMEM = "Stream error";
/**@}*/

/** Ask user if they want to allow some action.
  * \param command The action to ask the user about. See #AskUserCommandEnum.
  * \return false if the user accepted, true if the user denied.
  */
bool userDenied(AskUserCommand command)
{
	uint8_t i;
	bool r; // what will be returned

	clearLcd();

	r = true;
	if (command == ASKUSER_NUKE_WALLET)
	{
		waitForNoButtonPress();
		gotoStartOfLine(0);
		writeString(str_delete_line0, true);
		gotoStartOfLine(1);
		writeString(str_delete_line1, true);
		r = waitForButtonPress();
	}
	else if (command == ASKUSER_NEW_ADDRESS)
	{
		waitForNoButtonPress();
		gotoStartOfLine(0);
		writeString(str_new_line0, true);
		gotoStartOfLine(1);
		writeString(str_new_line1, true);
		r = waitForButtonPress();
	}
	else if (command == ASKUSER_SIGN_TRANSACTION)
	{
		for (i = 0; i < list_index; i++)
		{
			clearLcd();
			waitForNoButtonPress();
			gotoStartOfLine(0);
			writeString(str_sign_part0, true);
			writeString(list_amount[i], false);
			writeString(str_sign_part1, true);
			gotoStartOfLine(1);
			writeString(list_address[i], false);
			r = waitForButtonPress();
			if (r)
			{
				// All outputs must be approved in order for a transaction
				// to be signed. Thus if the user denies spending to one
				// output, the entire transaction is forfeit.
				break;
			}
		}
		if (!r && transaction_fee_set)
		{
			clearLcd();
			waitForNoButtonPress();
			gotoStartOfLine(0);
			writeString(str_fee_part0, true);
			gotoStartOfLine(1);
			writeString(transaction_fee_amount, false);
			writeString(str_fee_part1, true);
			r = waitForButtonPress();
		}
	}
	else if (command == ASKUSER_FORMAT)
	{
		waitForNoButtonPress();
		gotoStartOfLine(0);
		writeString(str_format_line0, true);
		gotoStartOfLine(1);
		writeString(str_format_line1, true);
		r = waitForButtonPress();
	}
	else if (command == ASKUSER_CHANGE_NAME)
	{
		waitForNoButtonPress();
		gotoStartOfLine(0);
		writeString(str_change_name_line0, true);
		gotoStartOfLine(1);
		writeString(str_change_name_line1, true);
		r = waitForButtonPress();
	}
	else if (command == ASKUSER_BACKUP_WALLET)
	{
		waitForNoButtonPress();
		gotoStartOfLine(0);
		writeString(str_backup_line0, true);
		gotoStartOfLine(1);
		writeString(str_backup_line1, true);
		r = waitForButtonPress();
	}
	else if (command == ASKUSER_RESTORE_WALLET)
	{
		waitForNoButtonPress();
		gotoStartOfLine(0);
		writeString(str_restore_line0, true);
		gotoStartOfLine(1);
		writeString(str_restore_line1, true);
		r = waitForButtonPress();
	}
	else if (command == ASKUSER_CHANGE_KEY)
	{
		waitForNoButtonPress();
		gotoStartOfLine(0);
		writeString(str_change_key_line0, true);
		gotoStartOfLine(1);
		writeString(str_change_key_line1, true);
		r = waitForButtonPress();
	}
	else if (command == ASKUSER_GET_MASTER_KEY)
	{
		waitForNoButtonPress();
		gotoStartOfLine(0);
		writeString(str_get_master_key_line0, true);
		gotoStartOfLine(1);
		writeString(str_get_master_key_line1, true);
		r = waitForButtonPress();
	}
	else
	{
		waitForNoButtonPress();
		gotoStartOfLine(0);
		writeString(str_unknown_line0, true);
		gotoStartOfLine(1);
		writeString(str_unknown_line1, true);
		waitForButtonPress();
		r = true; // unconditionally deny
	}

	clearLcd();
	return r;
}

/** Convert 4 bit number into corresponding hexadecimal character. For
  * example, 0 is converted into '0' and 15 is converted into 'f'.
  * \param nibble The 4 bit number to look at. Only the least significant
  *               4 bits are considered.
  * \return The hexadecimal character.
  */
static char nibbleToHex(uint8_t nibble)
{
	uint8_t temp;
	temp = (uint8_t)(nibble & 0xf);
	if (temp < 10)
	{
		return (char)('0' + temp);
	}
	else
	{
		return (char)('a' + (temp - 10));
	}
}

/** First line of string which tells the user whether backup is encrypted
  * or not. */
static const char str_seed_encrypted_or_not_line0[] PROGMEM = "Backup is";
/** Second line of string which tells the user that the backup is
  * encrypted. */
static const char str_seed_encrypted_line1[] PROGMEM = "encrypted";
/** Second line of string which tells the user that the backup is not
  * encrypted. */
static const char str_seed_not_encrypted_line1[] PROGMEM = "not encrypted";

/** Write backup seed to some output device. The choice of output device and
  * seed representation is up to the platform-dependent code. But a typical
  * example would be displaying the seed as a hexadecimal string on a LCD.
  * \param seed A byte array of length #SEED_LENGTH bytes which contains the
  *             backup seed.
  * \param is_encrypted Specifies whether the seed has been encrypted.
  * \param destination_device Specifies which (platform-dependent) device the
  *                           backup seed should be sent to.
  * \return false on success, true if the backup seed could not be written
  *         to the destination device.
  */
bool writeBackupSeed(uint8_t *seed, bool is_encrypted, uint32_t destination_device)
{
	uint8_t i;
	uint8_t one_byte; // current byte of seed
	uint8_t byte_counter; // current byte on screen, 0 = first, 1 = second etc.
	char str[4];

	if (destination_device != 0)
	{
		return true;
	}

	// Tell user whether seed is encrypted or not.
	clearLcd();
	waitForNoButtonPress();
	gotoStartOfLine(0);
	writeString(str_seed_encrypted_or_not_line0, true);
	gotoStartOfLine(1);
	if (is_encrypted)
	{
		writeString(str_seed_encrypted_line1, true);
	}
	else
	{
		writeString(str_seed_not_encrypted_line1, true);
	}
	if (waitForButtonPress())
	{
		clearLcd();
		return true;
	}
	waitForNoButtonPress();

	// Output seed to LCD.
	// str is " xx", where xx are hexadecimal digits.
	str[0] = ' ';
	str[3] = '\0';
	byte_counter = 0;
	for (i = 0; i < SEED_LENGTH; i++)
	{
		one_byte = seed[i];
		str[1] = nibbleToHex((uint8_t)(one_byte >> 4));
		str[2] = nibbleToHex(one_byte);
		if (byte_counter == 12)
		{
			waitForNoButtonPress();
			if (waitForButtonPress())
			{
				clearLcd();
				return true;
			}
			clearLcd();
			byte_counter = 0;
		}
		// The following code will output the seed in the format:
		// " xxxx xxxx xxxx"
		// " xxxx xxxx xxxx"
		if (byte_counter == 0)
		{
			gotoStartOfLine(0);
		}
		else if (byte_counter == 6)
		{
			gotoStartOfLine(1);
		}
		if ((byte_counter & 1) == 0)
		{
			writeString(str, false);
		}
		else
		{
			// Don't include space.
			writeString(&(str[1]), false);
		}
		byte_counter++;
	}
	waitForNoButtonPress();
	if (waitForButtonPress())
	{
		clearLcd();
		return true;
	}
	clearLcd();
	return false;
}


/** Notify user of stream error via. LCD. */
void streamError(void)
{
	clearLcd();
	gotoStartOfLine(0);
	writeString(str_stream_error, true);
}
