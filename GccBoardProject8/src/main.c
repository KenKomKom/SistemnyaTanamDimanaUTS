#include <asf.h>
#include <util/delay.h>
#include <stdio.h>
#include <stdlib.h>

#define CLOSE_RANGE 3               // Jarak dekat maksimal (cm)
#define MID_RANGE 5                 // Jarak menengah maksimal (cm)
#define FAR_RANGE 8                 // Jarak jauh maksimal (cm)
#define MAX_RANGE 11              // Jarak untuk menghapus penguncian

#define BUZZER_PIN PIN0_bm          // Pin untuk buzzer pada PC0
#define CLOSE_BUZZER_DURATION 2500  // Durasi bunyi kontinu untuk jarak dekat (ms)

void setup_main_timer(void);
void setup_buzzer_timer(void);
long calculate_distance_cm(uint16_t pulse_duration);
void control_buzzer(long distance_cm);
void toggle_buzzer(void);                       // Fungsi untuk mengaktifkan buzzer dengan interrupt
uint16_t get_dynamic_delay(long distance_cm);   // Fungsi untuk menentukan delay dinamis
void delay_ms_runtime(uint16_t ms);             // Fungsi delay berbasis runtime

// Variabel status buzzer dan penguncian
volatile bool buzzer_active         = false;
volatile bool buzzer_triggered      = false;
volatile bool buzzer_locked         = false;
volatile bool close_range_active    = false;    // Menandakan mode jarak dekat aktif
volatile uint16_t buzzer_period     = 0;        // Interval delay untuk buzzer

int main(void)
{
	board_init();
	sysclk_init();
	gfx_mono_init();  // Inisialisasi LCD
	cpu_irq_enable();

	gpio_set_pin_high(NHD_C12832A1Z_BACKLIGHT); // Mengaktifkan backlight LCD

	setup_main_timer();
	setup_buzzer_timer();

	while (1)
	{
		uint16_t start_time = 0, end_time = 0;
		long distance_cm = 0;

		// Kirim trigger ke sensor dengan delay akurat
		PORTB.DIRSET = PIN0_bm;   // Set PB0 sebagai output untuk trigger
		PORTB.OUTCLR = PIN0_bm;
		_delay_us(2);
		PORTB.OUTSET = PIN0_bm;
		_delay_us(10);
		PORTB.OUTCLR = PIN0_bm;

		// Atur PB0 sebagai input untuk menerima echo
		PORTB.DIRCLR = PIN0_bm;

		// Tunggu sampai echo naik (rising edge)
		while (!(PORTB.IN & PIN0_bm)) {
			if (tc_read_count(&TCC0) > 60000) break;
		}
		start_time = tc_read_count(&TCC0);  // Catat waktu mulai

		// Tunggu sampai echo turun (falling edge)
		while (PORTB.IN & PIN0_bm) {
			if (tc_read_count(&TCC0) > 60000) break;
		}
		end_time = tc_read_count(&TCC0);  // Catat waktu selesai

		// Hitung durasi pulse
		if (end_time >= start_time) {
			distance_cm = calculate_distance_cm(end_time - start_time);
			} else {
			distance_cm = calculate_distance_cm((0xFFFF - start_time) + end_time);
		}

		// Tampilkan jarak di LCD
		char distance_str[10];
		dtostrf(distance_cm, 6, 2, distance_str); // Konversi jarak ke string
		char buffarray[200];
		snprintf(buffarray, sizeof(buffarray), "Jarak: %s cm    ", distance_str);
		gfx_mono_draw_string(buffarray, 0, 0, &sysfont);

		// Kontrol Buzzer Berdasarkan Jarak
		control_buzzer(distance_cm);

		// Dapatkan delay yang dinamis berdasarkan jarak sebelumnya
		uint16_t dynamic_delay = get_dynamic_delay(distance_cm);
		delay_ms_runtime(dynamic_delay);  // Delay antar pengukuran berdasarkan jarak
	}
}

long calculate_distance_cm(uint16_t pulse_duration)
{
	return (pulse_duration * 0.01715) / 2;
}

void setup_main_timer(void)
{
	tc_enable(&TCC0);
	tc_set_wgm(&TCC0, TC_WG_NORMAL);
	tc_write_period(&TCC0, 0xFFFF);
	tc_write_clock_source(&TCC0, TC_CLKSEL_DIV1_gc); // Tanpa prescaler, setiap tick 0.5 µs
}

void setup_buzzer_timer(void)
{
	tc_enable(&TCC1);
	tc_set_wgm(&TCC1, TC_WG_NORMAL);
	tc_write_clock_source(&TCC1, TC_CLKSEL_DIV64_gc); // Prescaler untuk kontrol frekuensi buzzer
	tc_set_overflow_interrupt_callback(&TCC1, toggle_buzzer);
	tc_set_overflow_interrupt_level(&TCC1, TC_INT_LVL_LO);
	pmic_enable_level(PMIC_LVL_LOW);
	PORTC.DIRSET = BUZZER_PIN;  // Set PC0 sebagai output untuk buzzer
}

// Fungsi kontrol buzzer berdasarkan jarak dan penguncian
void control_buzzer(long distance_cm)
{
	if (distance_cm > MAX_RANGE) {
		buzzer_locked = false;    // Reset penguncian jika di luar jarak maksimal
		buzzer_triggered = false; // Reset trigger
	}

	if (distance_cm > 0 && distance_cm <= CLOSE_RANGE && !buzzer_triggered && !buzzer_locked) {
		// Jarak dekat: bunyi kontinu selama 2,5 detik tanpa mengganggu timer
		buzzer_triggered = true;
		buzzer_locked = true;
		close_range_active = true;  // Aktifkan mode jarak dekat
		buzzer_active = false;      // Nonaktifkan toggle timer TCC1 sementara
		PORTC.OUTSET = BUZZER_PIN;  // Nyalakan buzzer kontinu

		// Timer delay tanpa mengganggu interrupt timer lain
		for (uint16_t i = 0; i < (CLOSE_BUZZER_DURATION / 10); i++) {
			_delay_ms(10);  // Durasi total 2,5 detik
		}
		
		PORTC.OUTCLR = BUZZER_PIN;   // Matikan buzzer setelah 2,5 detik
		close_range_active = false;  // Nonaktifkan mode jarak dekat
	}
	else if (!buzzer_locked && distance_cm > CLOSE_RANGE && distance_cm <= MID_RANGE) {
		// Jarak menengah: aktifkan buzzer dengan periode cepat
		buzzer_active = true;
		buzzer_period = 2000;  // Periode cepat (~1 ms)
		tc_write_period(&TCC1, buzzer_period);
	}
	else if (!buzzer_locked && distance_cm > MID_RANGE && distance_cm <= FAR_RANGE) {
		// Jarak jauh: aktifkan buzzer dengan periode menengah
		buzzer_active = true;
		buzzer_period = 5000;  // Periode menengah (~2.5 ms)
		tc_write_period(&TCC1, buzzer_period);
	}
	else if (!buzzer_locked && distance_cm > FAR_RANGE && distance_cm <= MAX_RANGE) {
		// Jarak jauh maksimum: aktifkan buzzer dengan periode lambat
		buzzer_active = true;
		buzzer_period = 7500;  // Periode lambat (~3.75 ms)
		tc_write_period(&TCC1, buzzer_period);
	}
	else {
		// Matikan buzzer jika di luar rentang atau jika dalam kondisi terkunci
		buzzer_active = false;
		PORTC.OUTCLR = BUZZER_PIN;
	}
}

// ISR untuk kontrol toggle buzzer menggunakan timer TCC1
void toggle_buzzer(void)
{
	if (buzzer_active && !close_range_active) {
		PORTC.OUTTGL = BUZZER_PIN;  // Toggle buzzer jika aktif dan tidak dalam mode jarak dekat
	}
}

// Fungsi untuk mendapatkan delay dinamis berdasarkan jarak
uint16_t get_dynamic_delay(long distance_cm)
{
	if (distance_cm <= CLOSE_RANGE) {
		return 50;  // Delay lebih pendek untuk jarak dekat
	}
	else if (distance_cm <= MID_RANGE) {
		return 150; // Delay menengah untuk jarak menengah
	}
	else if (distance_cm <= FAR_RANGE) {
		return 300; // Delay lebih lama untuk jarak jauh
	}
	else {
		return 500; // Delay maksimal untuk jarak di luar rentang
	}
}

// Fungsi delay runtime menggunakan loop
void delay_ms_runtime(uint16_t ms)
{
	while (ms--) {
		_delay_ms(1);  // Delay 1 ms pada setiap iterasi loop
	}
}
