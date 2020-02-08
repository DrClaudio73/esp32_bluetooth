#include "string.h"
#include "ctype.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

#include "driver/gpio.h"


// ---------- iBeacon packet structure ----------

typedef struct {
    //uint8_t flags[3];
    uint8_t length;
    uint8_t type;
    uint16_t company_id;
    uint16_t beacon_type;
}__attribute__((packed)) ibeacon_header_t;

typedef struct {
    uint8_t proximity_uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t measured_power;
}__attribute__((packed)) ibeacon_data_t;

typedef struct {
    ibeacon_header_t ibeacon_header;
    ibeacon_data_t ibeacon_data;
}__attribute__((packed)) ibeacon_packet_t;

// ----------------------------------------------

// iBeacon fixed header
ibeacon_header_t ibeacon_fixed_header = {
	//.flags = {0x02, 0x01, 0x06},
	.length = 0x1A,
    .type = 0xFF,
	.company_id = 0x004C,
    .beacon_type = 0x1502
};

// scan parameters
static esp_ble_scan_params_t ble_scan_params = {
	.scan_type              = BLE_SCAN_TYPE_ACTIVE,
	.own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
	.scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
	.scan_interval          = 0x50,
	.scan_window            = 0x30
};

// global variables
bool led_on = false;
static EventGroupHandle_t my_event_group;
const int FOUND_BIT = BIT0;

// timeout task
void timeout_task(void *pvParameter) {

	while(1) {
		// wait the timeout period
		EventBits_t uxBits;
		uxBits = xEventGroupWaitBits(my_event_group, FOUND_BIT, true, true, CONFIG_TIMEOUT / portTICK_PERIOD_MS); 

		// if found bit was not set, the function returned after the timeout period
		if((uxBits & FOUND_BIT) == 0) {

			// turn the led off
			if(led_on) {
				gpio_set_level(CONFIG_LED_PIN, 0);
				led_on = false;
				printf("iBeacon not found for %d milliseconds, led off\n", CONFIG_TIMEOUT);
				ESP_LOGE("OFF","iBeacon not found, led OFF\n");
			}
		}
	}
}

// GAP callback
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {

		case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: 

			printf("ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT\n");
			if(param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
				printf("Scan parameters set, start scan process\n\n");
				esp_ble_gap_start_scanning(0);
			}
			else printf("Unable to set scan parameters, error code %d\n\n", param->scan_param_cmpl.status);
			break;

		case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:

			printf("ESP_GAP_BLE_SCAN_START_COMPLETE_EVT\n");
			if(param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
				printf("Scan process started\n\n");

				// start the timeout task
				xTaskCreate(&timeout_task, "timeout_task", 2048, NULL, 5, NULL);
				printf("Timeout task started\n\n");
			}
			else printf("Unable to start scan process, error code %d\n\n", param->scan_start_cmpl.status);
			break;

		case ESP_GAP_BLE_SCAN_RESULT_EVT:

			if(param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {

				if (!((param->scan_rst.bda[0]==0x9c)||(param->scan_rst.bda[0]==0xe0))){
					printf("\r\n**************************************ESP_GAP_BLE_SCAN_RESULT_EVT******************************\n");
					printf("Device found: ADDR=");
					for(int i = 0; i < ESP_BD_ADDR_LEN; i++) {
						printf("%02X", param->scan_rst.bda[i]);
						if(i != ESP_BD_ADDR_LEN -1) printf(":");
					}
					printf("\r\ndev type: %u (ESP_BT_DEVICE_TYPE_BLE=2; ESP_BT_DEVICE_TYPE_DUMO = 0x03)\r\n",param->scan_rst.dev_type);
					printf("ble_addr_type: %u (BLE_ADDR_TYPE_PUBLIC = 0x00; BLE_ADDR_TYPE_RANDOM = 0x01)\r\n",param->scan_rst.ble_addr_type);
					printf("ble_evt_type: %u (ESP_BLE_EVT_CONN_ADV = 0x00; ESP_BLE_EVT_DISC_ADV = 0x02; ESP_BLE_EVT_NON_CONN_ADV = 0x03)\r\n",param->scan_rst.ble_evt_type);
					printf("rssi: %d\r\n",param->scan_rst.rssi);
					printf("adv_data_len: %u\r\n",param->scan_rst.adv_data_len);
					printf("scan_rsp_len: %u\r\n",param->scan_rst.scan_rsp_len);
					printf("ble_adv: ");
					for(int i = 0; i < ESP_BLE_ADV_DATA_LEN_MAX + ESP_BLE_SCAN_RSP_DATA_LEN_MAX; i++) {
						printf("%02X", param->scan_rst.ble_adv[i]);
						if(i != ESP_BLE_ADV_DATA_LEN_MAX + ESP_BLE_SCAN_RSP_DATA_LEN_MAX -1) printf(":");
					}
					printf("\r\nflag: %u\r\n",param->scan_rst.flag);
					printf("num_resps: %u\r\n",param->scan_rst.num_resps);
					printf("num_dis: %d\r\n",param->scan_rst.num_dis);


					// try to read the flags
					uint8_t *adv_flags = NULL;
					uint8_t adv_flags_len = 0;
					adv_flags = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_FLAG, &adv_flags_len);
					if(adv_flags) {
						printf("\nFLAGS=");
						for(int i = 0; i < adv_flags_len; i++) printf("%02X", adv_flags[i]);
					}
					
					// try to read the manufacturer specific data
					uint8_t *adv_msd = NULL;
					uint8_t adv_msd_len = 0;
					adv_msd = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &adv_msd_len);
					if(adv_msd) {
						printf("\nADV_MANUFACTURER_SPECIFIC_DATA=");
						for(int i = 0; i < adv_msd_len; i++) printf("-%02X", adv_msd[i]);
					}
					
					// try to read the Service UUIDs
					uint8_t *adv_SUUIDs = NULL;
					uint8_t adv_SUUIDs_len = 0;
					adv_SUUIDs = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_128SRV_PART, &adv_SUUIDs_len);
					if(adv_SUUIDs) {
						printf("\nService UUIDs=");
						for(int i = 0; i < adv_SUUIDs_len; i++) printf("-%02X", adv_SUUIDs[i]);
					}
					
					// try to read the complete name
					uint8_t *adv_name = NULL;
					uint8_t adv_name_len = 0;
					adv_name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
					if(adv_name) {
						printf("\nFULL NAME=");
						for(int i = 0; i < adv_name_len; i++) printf("%c", adv_name[i]);
					}
					
					// try to read the transmission power
					uint8_t *adv_txpwr = NULL;
					uint8_t adv_txpwr_len = 0;
					adv_txpwr = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_TX_PWR, &adv_txpwr_len);
					if(adv_txpwr) {
						printf("\nTX POWER=");
						for(int i = 0; i < adv_txpwr_len; i++) printf("%d", adv_txpwr[i]);
					}
					/* *********************************** */
					// a device was found, check if it's an iBeacon

					printf("\r\n");
				}
				// first verify that data length is 30 bytes
				if ((param->scan_rst.ble_adv != NULL) && (param->scan_rst.adv_data_len == 0x1B)) {
					// now compare the header with the iBeacon fixed header
					if (!memcmp(param->scan_rst.ble_adv, (uint8_t *)&ibeacon_fixed_header, sizeof(ibeacon_fixed_header))) {

						// this is an iBeacon, parse the data and check if the UUID matches and the signal strength is ok
						char ibeacon_uuid[32];
						ibeacon_packet_t *ibeacon_packet = (ibeacon_packet_t *)(param->scan_rst.ble_adv);
						printf("\nibeacon packet: ");
						for(int i = 0; i < 16; i++) {
							if (i) {
								printf("-");
							}
							printf("%02x", ibeacon_packet->ibeacon_data.proximity_uuid[i]);
							sprintf(ibeacon_uuid + 2*i, "%02x", ibeacon_packet->ibeacon_data.proximity_uuid[i]);
						}
						printf("\r\n");
						printf("ibeacon_uuid (as-is): %s",ibeacon_uuid);
						for (int i=0; i< 32 ; i++){
							ibeacon_uuid[i]=toupper(ibeacon_uuid[i]);
						}
						printf("\r\n");
						printf("ibeacon_uuid (uppercase): %s",ibeacon_uuid);
						printf("\r\n");
						
						if((strncmp(ibeacon_uuid, CONFIG_IBEACON_UUID, 32) == 0) && (param->scan_rst.rssi > CONFIG_MIN_RSSI)) {

							// if it matches, notify the timeout task and turn the led on
							xEventGroupSetBits(my_event_group, FOUND_BIT);

							if(!led_on) {
								gpio_set_level(CONFIG_LED_PIN, 1);
								led_on = true;
								ESP_LOGE("ON","iBeacon found, led on\n");
							}
						}
					}
				}
				if (!((param->scan_rst.bda[0]==0x9c)||(param->scan_rst.bda[0]==0xe0))){
					printf("\r\n*****************************END*****************************\r\n");
				}
			}
			else if(param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT)
				printf("Scan complete\n\n");
			break;

		default:

			printf("Event %d unhandled\n\n", event);
			break;
	}
}


void app_main() {

	printf("iBeacon finder\n\n");

	// set components to log only errors
	esp_log_level_set("*", ESP_LOG_ERROR);

	// initialize nvs
	ESP_ERROR_CHECK(nvs_flash_init());
	printf("- NVS init ok\n");

	// configure led pin
	gpio_pad_select_gpio(CONFIG_LED_PIN);
	gpio_set_direction(CONFIG_LED_PIN, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_LED_PIN, 0);
	printf("- LED pin configurated\n");

	// create the event group
	my_event_group = xEventGroupCreate();
	printf("- Event group created\n");

	// release memory reserved for classic BT (not used)
	ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
	printf("- Memory for classic BT released\n");

	// initialize the BT controller with the default config
	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	esp_bt_controller_init(&bt_cfg);
	printf("- BT controller init ok\n");

	// enable the BT controller in BLE mode
	esp_bt_controller_enable(ESP_BT_MODE_BLE);
	printf("- BT controller enabled in BLE mode\n");

	// initialize Bluedroid library
	esp_bluedroid_init();
	esp_bluedroid_enable();
	printf("- Bluedroid initialized and enabled\n");

	// register GAP callback function
	ESP_ERROR_CHECK(esp_ble_gap_register_callback(esp_gap_cb));
	printf("- GAP callback registered\n\n");
	// configure scan parameters
	esp_ble_gap_set_scan_params(&ble_scan_params);
}