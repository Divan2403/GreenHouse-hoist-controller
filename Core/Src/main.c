/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 3-phase PWM with deadtime, encoder (TIM2) and I2C LCD
  *                   - PC14: Start Forward (downwards)
  *                   - PC13: Start Reverse (upwards)
  *                   - TIM2 encoder: PA0 (CH1), PB3 (CH2)
  *                   - I2C1 LCD: PB6 (SCL), PB7 (SDA)
  *
  * Behavior:
  *  - Encoder used as limiter: encoderPos range 0..100 (as in current code)
  *  - If moving downwards (PC14 pressed) and encoderPos >= 100 -> immediate stop
  *  - If moving upwards (PC13 pressed) and encoderPos <= 10  -> immediate stop
  *  - PC15 is emergency stop toggle: press to enter E-STOP (immediate stop),
  *    press again to clear E-STOP and allow operation again.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "LCD_Dev.h"   /* Your LCD driver header (lcd_init, lcd_clear, lcd_put_cur, lcd_send_string) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2; /* encoder */
TIM_HandleTypeDef htim3;
I2C_HandleTypeDef hi2c1;

/* USER CODE BEGIN PV */
#define PWM_PERIOD_COUNTS   799U
#define SINE_TABLE_SIZE     1024U

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static float sine_table[SINE_TABLE_SIZE];
static volatile float angle_step = 1.0f;     // electrical step per update
static volatile uint16_t elec_angle = 0;
static volatile uint8_t pwm_enabled = 0;
static volatile uint8_t reverse_dir = 0;     // 0 = forward (A-B-C), 1 = reverse (A-C-B)

/* encoder position limited 0..100 in this version */
static volatile int32_t encoderPos = 0;
static volatile int32_t lcd_shown_pos = -999;

/* Emergency stop state: 0 = normal, 1 = E-STOP engaged */
static volatile uint8_t estop_active = 0;
/* previous sampled state of PC15 for edge detection */
static uint8_t prev_estop_btn = GPIO_PIN_RESET;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
static void MX_I2C1_Init(void);

static inline uint16_t SinToDuty(float x);
static void GenerateSineTable(void);
static inline uint32_t freq_to_period(float freq_hz);
static void Encoder_Process(void);
static void LCD_UpdatePosition(int32_t pos);

/* USER CODE BEGIN 0 */
static inline uint16_t SinToDuty(float x)
{
  float y = 0.5f * (x + 1.0f);
  float d = y * (float)PWM_PERIOD_COUNTS;
  if (d < 0.0f) d = 0.0f;
  if (d > (float)PWM_PERIOD_COUNTS) d = (float)PWM_PERIOD_COUNTS;
  return (uint16_t)d;
}

static void GenerateSineTable(void)
{
  for (uint32_t i = 0; i < SINE_TABLE_SIZE; ++i) {
    float angle = (2.0f * M_PI * (float)i) / (float)SINE_TABLE_SIZE;
    sine_table[i] = sinf(angle);
  }
}

/* TIM3 IRQ: update PWM duties (same logic you had) */
void TIM3_IRQHandler(void)
{
  if (__HAL_TIM_GET_FLAG(&htim3, TIM_FLAG_UPDATE) != RESET) {
    if (__HAL_TIM_GET_IT_SOURCE(&htim3, TIM_IT_UPDATE) != RESET) {
      __HAL_TIM_CLEAR_IT(&htim3, TIM_IT_UPDATE);

      if (pwm_enabled) {
        uint16_t a_deg = elec_angle;
        uint16_t b_deg, c_deg;

        if (reverse_dir == 0) {
          /* Normal direction (A-B-C) */
          b_deg = (uint16_t)((a_deg + 120U) % 360U);
          c_deg = (uint16_t)((a_deg + 240U) % 360U);
        } else {
          /* Reverse direction (A-C-B) */
          b_deg = (uint16_t)((a_deg + 240U) % 360U);
          c_deg = (uint16_t)((a_deg + 120U) % 360U);
        }

        uint32_t a_idx = (uint32_t)a_deg * SINE_TABLE_SIZE / 360U;
        uint32_t b_idx = (uint32_t)b_deg * SINE_TABLE_SIZE / 360U;
        uint32_t c_idx = (uint32_t)c_deg * SINE_TABLE_SIZE / 360U;

        TIM1->CCR1 = SinToDuty(sine_table[a_idx]);
        TIM1->CCR2 = SinToDuty(sine_table[b_idx]);
        TIM1->CCR3 = SinToDuty(sine_table[c_idx]);

        float na = (float)elec_angle + angle_step;
        while (na >= 360.0f) na -= 360.0f;
        elec_angle = (uint16_t)na;
      } else {
        /* Force safe neutral */
        TIM1->CCR1 = 0;
        TIM1->CCR2 = 0;
        TIM1->CCR3 = 0;
      }
    }
  }
}

/* freq -> TIM3 ARR mapping (unchanged) */
static inline uint32_t freq_to_period(float freq_hz)
{
  if (freq_hz <= 0.0f) return 1U;
  float period = 2634.5f * powf(freq_hz, -0.995f);
  if (period < 1.0f) period = 1.0f;
  return (uint32_t)period;
}

/* Read TIM2 counter and update a software-limited position 0..100 */
static void Encoder_Process(void)
{
  static uint16_t last_cnt = 0;
  uint16_t cnt = (uint16_t)__HAL_TIM_GET_COUNTER(&htim2);
  int32_t delta = (int32_t)cnt - (int32_t)last_cnt;

  /* wrap handling for 16-bit */
  if (delta > 32767) delta -= 65536;
  else if (delta < -32768) delta += 65536;

  if (delta != 0) {
    /* you can scale delta to change sensitivity; currently 1 tick -> 1 unit */
    encoderPos += delta;
    if (encoderPos < 0) encoderPos = 0;
    if (encoderPos > 100) encoderPos = 100;
    last_cnt = cnt;
  }
}

/* Simple LCD update (only when changed) */
static void LCD_UpdatePosition(int32_t pos)
{
  if (pos == lcd_shown_pos) return;
  char buf[20];
  lcd_clear();
  lcd_put_cur(0, 0);
  lcd_send_string("Encoder:");
  lcd_put_cur(1, 0);
  snprintf(buf, sizeof(buf), "Pos: %2ld/100", (long)pos);
  lcd_send_string(buf);
  lcd_shown_pos = pos;
}

/* Debounced edge detect for PC15 - call inside main loop */
static void EStop_Process(float *curr_freq)
{
  uint8_t btn = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_15);
  /* Detect rising edge (button press) */
  if (btn == GPIO_PIN_SET && prev_estop_btn == GPIO_PIN_RESET) {
    /* basic debounce: wait 40 ms and re-sample */
    HAL_Delay(40);
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_15) == GPIO_PIN_SET) {
      /* Toggle estop */
      estop_active = !estop_active;
      if (estop_active) {
        /* immediately stop motor (same behavior as hitting a limit) */
        pwm_enabled = 0;
        *curr_freq = 0.0f;
        TIM1->CCR1 = 0;
        TIM1->CCR2 = 0;
        TIM1->CCR3 = 0;
      } else {
        /* clearing E-STOP does NOT automatically start motion;
           user must press PC13/PC14 to start again */
      }
    }
  }
  prev_estop_btn = btn;
}

/* USER CODE END 0 */

/* Main ---------------------------------------------------------------------*/
int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_I2C1_Init();   /* initialize I2C for LCD */
  MX_TIM1_Init();
  MX_TIM3_Init();
  MX_TIM2_Init();   /* encoder */

  GenerateSineTable();

  /* Start LCD */
  lcd_init();
  lcd_clear();
  LCD_UpdatePosition(encoderPos);

  /* Start complementary PWM channels (will output neutral until CCR changes) */
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
  HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
  HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_2);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);
  HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_3);

  /* Start TIM3 periodic update interrupt */
  HAL_TIM_Base_Start_IT(&htim3);

  /* Start encoder interface (TIM2) */
  __HAL_TIM_SET_COUNTER(&htim2, 0U);
  HAL_TIM_Encoder_Start(&htim2, TIM_CHANNEL_ALL);

  float curr_freq = 0.0f;
  float target_freq = 0.0f;
  const float freq_step = 4.0f;   /* Hz per step */
  const uint32_t ramp_delay = 200; /* ms per step */

  while (1)
  {
    /* Always process encoder to update software limited encoderPos */
    Encoder_Process();
    LCD_UpdatePosition(encoderPos);

    /* Process emergency stop button (PC15) - can come from anywhere */
    EStop_Process(&curr_freq);

    /* If E-STOP active, block any attempts to enable PWM */
    if (estop_active) {
      target_freq = 0.0f;
      pwm_enabled = 0;
      /* show E-STOP on LCD optionally: (commented out to keep behavior identical)
         lcd_put_cur(0,9); lcd_send_string("E-STOP"); */
      HAL_Delay(ramp_delay);
      continue;
    }

    uint8_t btnForward = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_14); /* downwards */
    uint8_t btnReverse = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13); /* upwards */

    /* Immediate limit enforcement (Option 1 - immediate stop) */
    if (btnForward == GPIO_PIN_SET && encoderPos >= 100) {
      /* If trying to move down and at or beyond lower limit, stop immediately */
      target_freq = 0.0f;
      pwm_enabled = 0;
    }

    if (btnReverse == GPIO_PIN_SET && encoderPos <= 10) {
      /* If trying to move up and at or beyond upper limit, stop immediately */
      target_freq = 0.0f;
      pwm_enabled = 0;
    }

    /* Determine target frequency & direction (only enable if not blocked by limits) */
    if (btnForward == GPIO_PIN_SET) {
      /* only start forward if not at lower limit */
      if (encoderPos < 100) {
        pwm_enabled = 1;
        reverse_dir = 0; /* forward (downwards) */
        target_freq = 25.0f;
      } else {
        target_freq = 0.0f; /* blocked */
      }
    }
    else if (btnReverse == GPIO_PIN_SET) {
      /* only start reverse if not at upper limit */
      if (encoderPos > 10) {
        pwm_enabled = 1;
        reverse_dir = 1; /* reverse (upwards) */
        target_freq = 25.0f;
      } else {
        target_freq = 0.0f; /* blocked */
      }
    }
    else {
      target_freq = 0.0f; /* No button → ramp down */
    }

    /* Ramp current frequency toward target (unchanged) */
    if (curr_freq < target_freq) {
      curr_freq += freq_step;
      if (curr_freq > target_freq) curr_freq = target_freq;
    } else if (curr_freq > target_freq) {
      curr_freq -= freq_step;
      if (curr_freq < target_freq) curr_freq = target_freq;
    }

    /* Update TIM3 ARR (controls update stepping) or stop PWM */
    if (curr_freq > 0.0f) {
      uint32_t arr = freq_to_period(curr_freq);
      if (arr == 0U) arr = 1U;
      __HAL_TIM_SET_AUTORELOAD(&htim3, arr);
    } else {
      pwm_enabled = 0;
      TIM1->CCR1 = 0;
      TIM1->CCR2 = 0;
      TIM1->CCR3 = 0;
    }

    HAL_Delay(ramp_delay);
  }
}

/* ---------------------------------------------------------------------------
   System and peripheral init functions below (GPIO, TIM1, TIM2, TIM3, I2C1)
   -------------------------------------------------------------------------*/

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|
                                RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
}

/* TIM1 init (3-phase complementary PWM + deadtime) */
static void MX_TIM1_Init(void)
{
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBDT = {0};

  __HAL_RCC_TIM1_CLK_ENABLE();

  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0;
  htim1.Init.CounterMode = TIM_COUNTERMODE_CENTERALIGNED1;
  htim1.Init.Period = PWM_PERIOD_COUNTS;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK) { Error_Handler(); }

  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK) { Error_Handler(); }

  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) { Error_Handler(); }

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK) { Error_Handler(); }

  /* PWM channel config (common) */
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;

  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) { Error_Handler(); }
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2) != HAL_OK) { Error_Handler(); }
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_3) != HAL_OK) { Error_Handler(); }

  /* Dead-time & break config */
  sBDT.OffStateRunMode = TIM_OSSR_DISABLE;
  sBDT.OffStateIDLEMode = TIM_OSSI_DISABLE;
  sBDT.LockLevel = TIM_LOCKLEVEL_OFF;
  sBDT.DeadTime = 20U; /* ~1.25 us @16 MHz */
  sBDT.BreakState = TIM_BREAK_DISABLE;
  sBDT.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBDT.AutomaticOutput = TIM_AUTOMATICOUTPUT_ENABLE;

  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBDT) != HAL_OK) { Error_Handler(); }

  HAL_TIM_MspPostInit(&htim1);
}

/* TIM3 init (periodic update timer) */
static void MX_TIM3_Init(void)
{
  __HAL_RCC_TIM3_CLK_ENABLE();

  htim3.Instance = TIM3;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.Prescaler = (uint32_t)(16000000U / 1000000U) - 1U; /* 1 MHz tick */
  htim3.Init.Period = 600U;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK) { Error_Handler(); }

  HAL_NVIC_SetPriority(TIM3_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(TIM3_IRQn);
}

/* TIM2 init in Encoder mode (PA0 = CH1, PB3 = CH2) */
static void MX_TIM2_Init(void)
{
  TIM_Encoder_InitTypeDef sEncoderConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_TIM2_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* PA0 -> TIM2_CH1 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF1_TIM2;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* PB3 -> TIM2_CH2 */
  GPIO_InitStruct.Pin = GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF1_TIM2;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 0xFFFF;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

  sEncoderConfig.EncoderMode = TIM_ENCODERMODE_TI12;
  sEncoderConfig.IC1Polarity = TIM_ICPOLARITY_RISING;
  sEncoderConfig.IC1Selection = TIM_ICSELECTION_DIRECTTI;
  sEncoderConfig.IC1Prescaler = TIM_ICPSC_DIV1;
  sEncoderConfig.IC1Filter = 0;
  sEncoderConfig.IC2Polarity = TIM_ICPOLARITY_RISING;
  sEncoderConfig.IC2Selection = TIM_ICSELECTION_DIRECTTI;
  sEncoderConfig.IC2Prescaler = TIM_ICPSC_DIV1;
  sEncoderConfig.IC2Filter = 0;

  if (HAL_TIM_Encoder_Init(&htim2, &sEncoderConfig) != HAL_OK) { Error_Handler(); }

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK) { Error_Handler(); }
}

/* I2C1 init for LCD (PB6=SCL PB7=SDA) */
static void MX_I2C1_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_I2C1_CLK_ENABLE();

  /* Configure PB6 PB7 as AF for I2C1 */
  GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;        /* open-drain */
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;     /* AF4 on F4 for I2C1 */
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK) { Error_Handler(); }
}

/* GPIO init (buttons + estop) */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* Configure PC13, PC14, PC15 as input with pulldown (buttons active HIGH) */
  GPIO_InitStruct.Pin = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* Note: do NOT reconfigure PB6/PB7 here — MX_I2C1_Init configures them */
}

/* Error handler */
void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}
