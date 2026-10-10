/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include "secrets.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum {
  ST_INIT = 0,
  ST_NORMAL,
  ST_WARNING,
  ST_INTERLOCK,
  ST_COMM_ERROR
} SysState;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define WIFI_SSID        SECRET_WIFI_SSID
#define WIFI_PW          SECRET_WIFI_PW
#define SERVER_IP        SECRET_SERVER_IP
#define SERVER_PORT      "5000"

#define SEND_INTERVAL_MS 5000
#define ADC_SAMPLE_MS    200
#define FAIL_LIMIT       3

#define ADC_WARN_ON      2000   /* NORMAL    -> WARNING   */
#define ADC_WARN_OFF     1900   /* WARNING   -> NORMAL    */
#define ADC_INTER_ON     3000   /* WARNING   -> INTERLOCK */
#define ADC_INTER_OFF    2800   /* INTERLOCK -> WARNING   */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint8_t esp_rx_data;
volatile uint8_t esp_ring[256];
volatile uint16_t rx_head = 0;
volatile uint16_t rx_tail = 0;

char esp_resp[512];
uint16_t esp_resp_len = 0;

char http_req[256];
char cmd_buf[64];

uint32_t led_tick  = 0;
uint32_t send_tick = 0;
uint32_t adc_tick  = 0;
uint8_t  fail_count = 0;

SysState level_state = ST_INIT;   /* ADC로 판정한 상태 */
SysState sys_state   = ST_INIT;   /* 최종 표시 상태 */
uint8_t  comm_error  = 0;
uint16_t last_adc    = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void pc_log(const char *msg);
void esp_send(const char *msg);
int esp_cmd(const char *cmd, uint32_t timeout_ms);
int esp_wait_for(const char *token, uint32_t timeout_ms);
int esp_wait_keep(const char *token, uint32_t timeout_ms);
int esp_send_data(const char *data, uint32_t timeout_ms);
uint16_t adc_read(void);
const char *state_name(SysState s);
SysState next_level(SysState cur, uint16_t adc);
void refresh_state(void);
uint32_t led_period_ms(SysState s);
void wifi_connect(void);
int wifi_is_connected(void);
int open_tcp(void);
int send_sensor(uint16_t value, const char *state);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void pc_log(const char *msg)
{
  HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
}

void esp_send(const char *msg)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
}

static void esp_drain(void)
{
  while (rx_tail != rx_head)
  {
    uint8_t c = esp_ring[rx_tail];
    rx_tail = (rx_tail + 1) % sizeof(esp_ring);
    HAL_UART_Transmit(&huart2, &c, 1, HAL_MAX_DELAY);

    if (esp_resp_len < sizeof(esp_resp) - 1)
    {
      esp_resp[esp_resp_len++] = (char)c;
      esp_resp[esp_resp_len] = '\0';
    }
  }
}

int esp_cmd(const char *cmd, uint32_t timeout_ms)
{
  esp_resp_len = 0;
  esp_resp[0] = '\0';
  rx_tail = rx_head;

  pc_log(">> ");
  pc_log(cmd);
  esp_send(cmd);

  uint32_t start = HAL_GetTick();
  while (HAL_GetTick() - start < timeout_ms)
  {
    esp_drain();

    if (strstr(esp_resp, "\r\nOK\r\n")) return 1;
    if (strstr(esp_resp, "ERROR") || strstr(esp_resp, "FAIL")) return 0;
  }
  return -1;
}

/* 버퍼를 지우지 않고 이어서 대기 (이미 도착한 응답을 놓치지 않기 위함) */
int esp_wait_keep(const char *token, uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  while (HAL_GetTick() - start < timeout_ms)
  {
    esp_drain();

    if (strstr(esp_resp, token)) return 1;
    if (strstr(esp_resp, "ERROR") || strstr(esp_resp, "FAIL")) return 0;
  }
  return -1;
}

int esp_wait_for(const char *token, uint32_t timeout_ms)
{
  esp_resp_len = 0;
  esp_resp[0] = '\0';
  return esp_wait_keep(token, timeout_ms);
}

int esp_send_data(const char *data, uint32_t timeout_ms)
{
  rx_tail = rx_head;
  esp_resp_len = 0;
  esp_resp[0] = '\0';

  esp_send(data);
  return esp_wait_for("SEND OK", timeout_ms);
}

uint16_t adc_read(void)
{
  uint32_t v = 0;

  HAL_ADC_Start(&hadc1);
  if (HAL_ADC_PollForConversion(&hadc1, 100) == HAL_OK)
  {
    v = HAL_ADC_GetValue(&hadc1);
  }
  HAL_ADC_Stop(&hadc1);

  return (uint16_t)v;
}

const char *state_name(SysState s)
{
  switch (s)
  {
    case ST_INIT:       return "INIT";
    case ST_NORMAL:     return "NORMAL";
    case ST_WARNING:    return "WARNING";
    case ST_INTERLOCK:  return "INTERLOCK";
    case ST_COMM_ERROR: return "COMM_ERROR";
    default:            return "INIT";
  }
}

/* 히스테리시스를 적용한 상태 전이 */
SysState next_level(SysState cur, uint16_t adc)
{
  switch (cur)
  {
    case ST_INTERLOCK:
      if (adc >= ADC_INTER_OFF) return ST_INTERLOCK;
      return (adc >= ADC_WARN_ON) ? ST_WARNING : ST_NORMAL;

    case ST_WARNING:
      if (adc >= ADC_INTER_ON) return ST_INTERLOCK;
      if (adc <  ADC_WARN_OFF) return ST_NORMAL;
      return ST_WARNING;

    default:
      if (adc >= ADC_INTER_ON) return ST_INTERLOCK;
      if (adc >= ADC_WARN_ON)  return ST_WARNING;
      return ST_NORMAL;
  }
}

/* 상태가 바뀔 때만 로그 출력 */
void refresh_state(void)
{
  SysState next = comm_error ? ST_COMM_ERROR : level_state;

  if (next != sys_state)
  {
    char line[64];
    snprintf(line, sizeof(line), "\r\n[STATE] %s -> %s\r\n",
             state_name(sys_state), state_name(next));
    pc_log(line);
    sys_state = next;
  }
}

uint32_t led_period_ms(SysState s)
{
  switch (s)
  {
    case ST_NORMAL:     return 1000;
    case ST_WARNING:    return 400;
    case ST_INTERLOCK:  return 100;
    case ST_COMM_ERROR: return 2000;
    default:            return 1000;
  }
}

void wifi_connect(void)
{
  esp_cmd("AT\r\n", 1000);
  esp_cmd("AT+CWMODE=1\r\n", 2000);
  esp_cmd("AT+CWJAP_CUR=\"" WIFI_SSID "\",\"" WIFI_PW "\"\r\n", 30000);
  esp_cmd("AT+CIFSR\r\n", 2000);
  esp_cmd("AT+CIPMUX=0\r\n", 2000);
}

/* 응답에 SSID가 있으면 연결된 것으로 판단 (펌웨어별 응답 차이 대비) */
int wifi_is_connected(void)
{
  int r = esp_cmd("AT+CWJAP?\r\n", 3000);
  if (r != 1) return 0;
  if (strstr(esp_resp, "No AP")) return 0;
  return (strstr(esp_resp, WIFI_SSID) != NULL);
}

int open_tcp(void)
{
  const char *cmd = "AT+CIPSTART=\"TCP\",\"" SERVER_IP "\"," SERVER_PORT "\r\n";

  int r = esp_cmd(cmd, 5000);
  if (r == 1) return 1;

  /* 이전 연결이 남아 있으면 닫고 한 번 더 시도 */
  if (strstr(esp_resp, "ALREADY"))
  {
    esp_cmd("AT+CIPCLOSE\r\n", 2000);
    r = esp_cmd(cmd, 5000);
    return (r == 1);
  }

  return 0;
}

int send_sensor(uint16_t value, const char *state)
{
  int ok = 0;

  if (!open_tcp())
    return 0;

  snprintf(http_req, sizeof(http_req),
           "GET /data?value=%u&state=%s HTTP/1.1\r\n"
           "Host: " SERVER_IP ":" SERVER_PORT "\r\n"
           "Connection: close\r\n"
           "\r\n",
           (unsigned)value, state);

  snprintf(cmd_buf, sizeof(cmd_buf), "AT+CIPSEND=%d\r\n", (int)strlen(http_req));

  rx_tail = rx_head;
  pc_log(">> ");
  pc_log(cmd_buf);
  esp_send(cmd_buf);

  if (esp_wait_for(">", 3000) == 1 &&
      esp_send_data(http_req, 5000) == 1)
  {
    if (strstr(esp_resp, "200 OK"))
      ok = 1;
    else
      ok = (esp_wait_keep("200 OK", 5000) == 1);

    esp_wait_keep("CLOSED", 2000);
  }

  /* 실패하면 연결을 정리해서 다음 주기에 stale 상태가 남지 않게 함 */
  if (!ok)
    esp_cmd("AT+CIPCLOSE\r\n", 2000);

  return ok;
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_USART1_UART_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  HAL_UART_Receive_IT(&huart1, &esp_rx_data, 1);

  pc_log("BOOT OK\r\n");
  HAL_Delay(3000);

  wifi_connect();

  pc_log("\r\n=== READY ===\r\n");

  last_adc    = adc_read();
  level_state = next_level(ST_INIT, last_adc);
  refresh_state();

  adc_tick  = HAL_GetTick();
  led_tick  = HAL_GetTick();
  send_tick = HAL_GetTick() - SEND_INTERVAL_MS;   /* 부팅 직후 바로 1회 전송 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    while (rx_tail != rx_head)
    {
      uint8_t c = esp_ring[rx_tail];
      rx_tail = (rx_tail + 1) % sizeof(esp_ring);
      HAL_UART_Transmit(&huart2, &c, 1, HAL_MAX_DELAY);
    }

    /* 1) 200ms마다 ADC 샘플링 + 상태 판정 */
    if (HAL_GetTick() - adc_tick >= ADC_SAMPLE_MS)
    {
      adc_tick = HAL_GetTick();

      last_adc    = adc_read();
      level_state = next_level(level_state, last_adc);
      refresh_state();
    }

    /* 2) 상태별 LED 점멸 */
    if (HAL_GetTick() - led_tick >= led_period_ms(sys_state))
    {
      HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
      led_tick = HAL_GetTick();
    }

    /* 3) 5초마다 서버 전송 */
    if (HAL_GetTick() - send_tick >= SEND_INTERVAL_MS)
    {
      send_tick = HAL_GetTick();

      const char *state = state_name(level_state);

      char line[80];
      snprintf(line, sizeof(line), "\r\n[SENSOR] ADC=%u STATE=%s\r\n",
               (unsigned)last_adc, state);
      pc_log(line);

      if (send_sensor(last_adc, state))
      {
        fail_count = 0;
        comm_error = 0;
        pc_log("\r\n[HTTP] OK\r\n");
      }
      else
      {
        fail_count++;
        pc_log("\r\n[HTTP] FAIL\r\n");

        if (fail_count >= FAIL_LIMIT)
        {
          comm_error = 1;
          refresh_state();   /* 재접속 전에 COMM_ERROR를 먼저 표시 */

          if (!wifi_is_connected())
          {
            pc_log("\r\n[RECONNECT] WIFI\r\n");
            wifi_connect();
          }
          else
          {
            pc_log("\r\n[SERVER] DOWN (WIFI OK)\r\n");
          }
          fail_count = 0;
        }
      }

      refresh_state();
      send_tick = HAL_GetTick();
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    esp_ring[rx_head] = esp_rx_data;
    rx_head = (rx_head + 1) % sizeof(esp_ring);
    HAL_UART_Receive_IT(&huart1, &esp_rx_data, 1);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    __HAL_UART_CLEAR_OREFLAG(huart);
    HAL_UART_Receive_IT(&huart1, &esp_rx_data, 1);
  }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
