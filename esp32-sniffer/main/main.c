#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stddef.h>
#include <_ansi.h>

#include "driver/gpio.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_system.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "apps/sntp/sntp.h"

#include "md5.h"
#include "mqtt_client.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

/* LED RED ON: ESP32 turned on
 * LED BLUE FAST BLINK: startup phase
 * LED BLUE ON: ESP32 connected to the wifi, but not to the MQTT broker
 * LED BLUE BLINK: ESP32 connected to the broker */

/* --- LED variables --- */
#define BLINK_GPIO 2 //LED pin definition
#define BLINK_MODE 0
#define ON_MODE 1
#define OFF_MODE 2
#define STARTUP_MODE 3
static int BLINK_TIME_ON = 5; //LED blink time init on
static int BLINK_TIME_OFF = 1000; //LED blink time init off

 /* --- Some configurations --- */
#define SSID_MAX_LEN (32+1) //max length of a SSID
#define MD5_LEN (32+1) //length of md5 hash
#define BUFFSIZE 1024 //size of buffer used to send data to the server
#define NROWS 11 //max rows that buffer can have inside send_data, it can be changed modifying BUFFSIZE
#define MAX_FILES 3 //max number of files in SPIFFS partition

/* TAG of ESP32 for I/O operation */
static const char *TAG = "ETS";
/* Always set as true, when a fatal error occurs in task the variable will be set as false */
static bool RUNNING = true;
/* Only used in startup: if time_init() can't set current time for the first time -> reboot() */
static bool ONCE = true;
/* True if ESP is connected to the wifi, false otherwise */
static bool WIFI_CONNECTED = false;
/* True if ESP is connected to the MQTT broker, false otherwise */
static bool MQTT_CONNECTED = false;
/* If the variable is true the sniffer_task() will write on FILENAME1, otherwise on FILENAME2
 * The value of this variable is changed only by the function send_data() */
static bool WHICH_FILE = false;
 /* True when the wifi-task lock a file (to be send) and set the other file for the sniffer-task*/
static bool FILE_CHANGED = true;
/* Lock used for mutual exclusion for I/O operation in the files */
static _lock_t lck_file;
/* Lock used for MQTT connection to access to the MQTT_CONNECTED variable */
static _lock_t lck_mqtt;

/* Handle for blink task */
static TaskHandle_t xHandle_led = NULL;
/* Handle for sniff task */
static TaskHandle_t xHandle_sniff = NULL;
/* Handle for wifi task */
static TaskHandle_t xHandle_wifi = NULL;
/* Client variable for MQTT connection */
static esp_mqtt_client_handle_t client;
/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

typedef struct {
	int16_t fctl; //frame control
	int16_t duration; //duration id
	uint8_t da[6]; //receiver address
	uint8_t sa[6]; //sender address
	uint8_t bssid[6]; //filtering address
	int16_t seqctl; //sequence control
	unsigned char payload[]; //network data
} __attribute__((packed)) wifi_mgmt_hdr;

static esp_err_t event_handler(void *ctx, system_event_t *event);
static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event);

static void vfs_spiffs_init(void);
static void time_init(void);
static void initialize_sntp(void);
static void obtain_time(void);

static void blink_task(void *pvParameter);
static void set_blink_led(int state);

static void sniffer_task(void *pvParameter);
static void wifi_sniffer_init(void);
static void wifi_sniffer_deinit(void);
static void wifi_sniffer_packet_handler(void *buff, wifi_promiscuous_pkt_type_t type);
static void get_hash(unsigned char *data, int len_res, char hash[MD5_LEN]);
static void get_ssid(unsigned char *data, char ssid[SSID_MAX_LEN], uint8_t ssid_len);
static int get_sn(unsigned char *data);
static void get_ht_capabilites_info(unsigned char *data, char htci[5], int pkt_len, int ssid_len);
static void dumb(unsigned char *data, int len);
static void save_pkt_info(uint8_t address[6], char *ssid, time_t timestamp, char *hash, int8_t rssi, int sn, char htci[5]);
static int get_start_timestamp(void);

static void wifi_task(void *pvParameter);
static void wifi_connect_init(void);
static void wifi_connect_deinit(void);
static void mqtt_app_start(void);
static int set_waiting_time(void);
static void send_data(void);
static void file_init(char *filename);

static void reboot(char *msg_err); //called only by main thread

void app_main(void)
{
	ESP_LOGI(TAG, "[+] Startup...");

	ESP_ERROR_CHECK(nvs_flash_init()); //initializing NVS (Non-Volatile Storage)
	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL)); //initialize (wifi) event handler

	ESP_LOGI(TAG, "[!] Starting blink task...");
	xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE, NULL, 5, &xHandle_led);
	if(xHandle_led == NULL)
		reboot("Impossible to create LED task");

	set_blink_led(STARTUP_MODE);

    wifi_connect_init(); //both soft-AP and station

    if(CONFIG_VERBOSE){
    	tcpip_adapter_ip_info_t ip_info;
        uint8_t l_Mac[6];

        esp_wifi_get_mac(ESP_IF_WIFI_STA, l_Mac);
        ESP_LOGI(TAG, "MAC Address: %02x:%02x:%02x:%02x:%02x:%02x", l_Mac[0], l_Mac[1], l_Mac[2], l_Mac[3], l_Mac[4], l_Mac[5]);

    	ESP_ERROR_CHECK(tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info));
    	ESP_LOGI(TAG, "IP Address:  %s", ip4addr_ntoa(&ip_info.ip));
    	ESP_LOGI(TAG, "Subnet mask: %s", ip4addr_ntoa(&ip_info.netmask));
    	ESP_LOGI(TAG, "Gateway:     %s", ip4addr_ntoa(&ip_info.gw));

    	ESP_LOGI(TAG, "Free memory: %d bytes", esp_get_free_heap_size());
    	ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    	esp_log_level_set("*", ESP_LOG_INFO);
    	esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    	esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    	esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    	esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    	//Easter egg
    	printf("[---] Malnati please give us 9 points [---]\n");
    }

	vfs_spiffs_init(); //initializing virtual file system (SPI Flash File System)
	time_init(); //initializing time (current data time)

	_lock_init(&lck_file);
	_lock_init(&lck_mqtt);
	file_init(CONFIG_FILENAME1);
	file_init(CONFIG_FILENAME2);

	ESP_LOGI(TAG, "[!] Starting sniffing task...");
	xTaskCreate(&sniffer_task, "sniffig_task", 10000, NULL, 1, &xHandle_sniff);
	if(xHandle_sniff == NULL)
		reboot("Impossible to create sniffing task");

	ESP_LOGI(TAG, "[!] Starting Wi-Fi task...");
	xTaskCreate(&wifi_task, "wifi_task", 10000, NULL, 1, &xHandle_wifi);
	if(xHandle_wifi == NULL)
		reboot("Impossible to create Wi-Fi task");

	while(RUNNING){ //every 0.5s check if fatal error occurred
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}

	ESP_LOGW(TAG, "Deleting led task...");
	vTaskDelete(xHandle_led);
	ESP_LOGW(TAG, "Deleting sniffing task...");
	vTaskDelete(xHandle_sniff);
	ESP_LOGW(TAG, "Deleting Wi-Fi task...");
	vTaskDelete(xHandle_wifi);

	ESP_LOGW(TAG, "Unmounting SPIFFS");
	esp_vfs_spiffs_unregister(NULL); //SPIFFS unmounted

	ESP_LOGW(TAG, "Stopping sniffing mode...");
	wifi_sniffer_deinit();
	ESP_LOGI(TAG, "Stopped");

	ESP_LOGW(TAG, "Disconnecting from %s...", CONFIG_WIFI_SSID);
	wifi_connect_deinit();

	reboot("Rebooting: Fatal error occurred in a task");
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
	switch(event->event_id){
		case SYSTEM_EVENT_STA_START:
	    	ESP_LOGI(TAG, "[WI-FI] Connecting to %s", CONFIG_WIFI_SSID);
	    	ESP_ERROR_CHECK(esp_wifi_connect());
			break;

		case SYSTEM_EVENT_STA_GOT_IP: //wifi connected
			ESP_LOGI(TAG, "[WI-FI] Connected");
			WIFI_CONNECTED = true;
			set_blink_led(ON_MODE);
			xEventGroupSetBits(wifi_event_group, BIT0);
			break;

		case SYSTEM_EVENT_STA_DISCONNECTED: //wifi lost connection
			ESP_LOGI(TAG, "[WI-FI] Disconnected");
			if(WIFI_CONNECTED == false)
				ESP_LOGW(TAG, "[WI-FI] Impossible to connect to wifi: wrong password and/or SSID or Wi-Fi down");
			WIFI_CONNECTED = false;
			set_blink_led(OFF_MODE);
			if(RUNNING){
				ESP_ERROR_CHECK(esp_wifi_connect());
			}
			else
				xEventGroupClearBits(wifi_event_group, BIT0);
			break;

		default:
			break;
	}

	return ESP_OK;
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    client = event->client;

    //your_context_t *context = event->context;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "[MQTT] Connected");

        	_lock_acquire(&lck_mqtt);
            MQTT_CONNECTED = true;
        	_lock_release(&lck_mqtt);

			set_blink_led(BLINK_MODE);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "[MQTT] Disconnected");

            _lock_acquire(&lck_mqtt);
            MQTT_CONNECTED = false;
        	_lock_release(&lck_mqtt);

        	set_blink_led(ON_MODE);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "[MQTT] EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "[MQTT] EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "[MQTT] EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "[MQTT] EVENT_DATA");
            ESP_LOGI(TAG, "[MQTT] TOPIC=%.*s\r\n", event->topic_len, event->topic);
            ESP_LOGI(TAG, "[MQTT] DATA=%.*s\r\n", event->data_len, event->data);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "[MQTT] MQTT_EVENT_ERROR");
            break;
    }

    return ESP_OK;
}

static void blink_task(void *pvParameter)
{
    gpio_pad_select_gpio(BLINK_GPIO);

    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    while(true){
        /* Blink off (output low) */
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(BLINK_TIME_OFF / portTICK_PERIOD_MS);

        /* Blink on (output high) */
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(BLINK_TIME_ON / portTICK_PERIOD_MS);
    }
}

static void set_blink_led(int state)
{
	switch(state){
		case BLINK_MODE: //blink
			BLINK_TIME_OFF = 1000;
			BLINK_TIME_ON = 1000;
			break;
		case ON_MODE: //always on
			BLINK_TIME_OFF = 5;
			BLINK_TIME_ON = 2000;
			break;
		case OFF_MODE: //always off
			BLINK_TIME_OFF = 2000;
			BLINK_TIME_ON = 5;
			break;
		case STARTUP_MODE: //fast blink
			BLINK_TIME_OFF = 100;
			BLINK_TIME_ON = 100;
			break;
		default:
			break;
	}
}

static void vfs_spiffs_init()
{
    esp_vfs_spiffs_conf_t conf = {
    		.base_path = "/spiffs",
			.partition_label = NULL,
			.max_files = MAX_FILES,
			.format_if_mount_failed = true
    };

    //esp_vfs_spiffs_register() is an all-in-one convenience function
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if(ret != ESP_OK){
        if(ret == ESP_FAIL){
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if(ret == ESP_ERR_NOT_FOUND){
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        }
        else{
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        reboot("Fatal error SPIFFS");
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }
    else{
        ESP_LOGI(TAG, "[SPIFFS] Partition size: total: %d, used: %d", total, used);
    }
}

static void time_init()
{
	time_t now;
    struct tm timeinfo;
    char strftime_buf[64];

	ESP_LOGI(TAG, "Connecting to WiFi and getting time over NTP.");
	obtain_time();
	time(&now);  //update 'now' variable with current time

    //setting timezone to Greenwich
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "TIME INFO: The Greenwich date/time is: %s", strftime_buf);
}

static void obtain_time()
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 15;

    initialize_sntp();

    //wait for time to be set
    while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if(retry >= retry_count){ //can't set time
    	if(ONCE) //if it is first time -> reboot: no reason to sniff with wrong time
    		reboot("No response from server after several time. Impossible to set current time");
    }
    else{
    	ONCE = false;
    }
}

static void initialize_sntp()
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL); //automatically request time after 1h
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static void file_init(char *filename)
{
	FILE *fp = fopen(filename, "wb");

	if(fp == NULL){
		RUNNING = false;
		ESP_LOGE(TAG, "Error creating or initializing file %s", filename);
		return;
	}

	ESP_LOGI(TAG, "File %s initialized", filename);

	fclose(fp);
}

static void mqtt_app_start()
{
	// Wait for WiFi to be connected before starting MQTT
	ESP_LOGI(TAG, "[MQTT] Waiting for WiFi connection...");
	while(!WIFI_CONNECTED) {
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
	
	// Additional delay to ensure WiFi is stable
	vTaskDelay(1000 / portTICK_PERIOD_MS);

	ESP_LOGI(TAG, "[MQTT] MQTT temporarily disabled to test sniffer functionality");
	ESP_LOGW(TAG, "[MQTT] Sniffer will work but data won't be sent via MQTT");
	
	// For now, just set MQTT as not connected so the sniffer can work
	_lock_acquire(&lck_mqtt);
	MQTT_CONNECTED = false;
	_lock_release(&lck_mqtt);
	
	// TODO: Fix MQTT configuration issue and re-enable
	/*
	ESP_LOGI(TAG, "[MQTT] Initializing MQTT client...");
	
	// Simple static configuration to avoid dynamic allocation issues
	static const esp_mqtt_client_config_t mqtt_cfg = {
		.host = "192.168.1.126",
		.port = 1884,
		.client_id = "ESP32WROOM",
		.transport = MQTT_TRANSPORT_OVER_TCP,
		.keepalive = 120,
		.disable_auto_reconnect = false,
		.event_handle = mqtt_event_handler,
	};

	client = esp_mqtt_client_init(&mqtt_cfg);
	if(client == NULL) {
		ESP_LOGE(TAG, "[MQTT] Failed to initialize MQTT client");
		RUNNING = false;
		return;
	}
	
	esp_err_t ret = esp_mqtt_client_start(client);
	if(ret != ESP_OK) {
		ESP_LOGE(TAG, "[MQTT] Failed to start MQTT client: %s", esp_err_to_name(ret));
		RUNNING = false;
		return;
	}

	ESP_LOGI(TAG, "[MQTT] Connecting to 192.168.1.126:1884");
	*/
}

static void wifi_connect_init()
{
	esp_log_level_set("wifi", ESP_LOG_NONE); //disable the default wifi logging

	tcpip_adapter_init();
	wifi_event_group = xEventGroupCreate(); //create the event group to handle wifi events

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); //create soft-AP and station control block

	wifi_config_t wifi_config = {
		.sta = {
			.ssid = CONFIG_WIFI_SSID,
			.password = CONFIG_WIFI_PSW,
			//.bssid_set = false,
		},
	};
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for connection to the WiFi network...");
    xEventGroupWaitBits(wifi_event_group, BIT0, false, true, portMAX_DELAY);
}

static void wifi_connect_deinit()
{
	ESP_ERROR_CHECK(esp_wifi_disconnect()); //disconnect the ESP32 WiFi station from the AP
	ESP_ERROR_CHECK(esp_wifi_stop()); //it stop station and free station control block
	ESP_ERROR_CHECK(esp_wifi_deinit()); //free all resource allocated in esp_wifi_init and stop WiFi task
}

static void wifi_task(void *pvParameter)
{
	#ifdef CONFIG_SNIFFING_TIME
		int st = CONFIG_SNIFFING_TIME*1000;
	#else
		int st = 60*1000; // Default to 60 seconds
	#endif

	ESP_LOGI(TAG, "[WIFI] Wi-Fi task created");

	mqtt_app_start();

	while(true){
		st = set_waiting_time(); //wait until the current minute ends
		vTaskDelay(st / portTICK_PERIOD_MS);

		_lock_acquire(&lck_mqtt);
		if(MQTT_CONNECTED) {
			send_data();
		} else {
			ESP_LOGW(TAG, "[WI-FI] MQTT not connected - data will accumulate in files");
		}
		_lock_release(&lck_mqtt);
	}
}

static int set_waiting_time()
{
	int st;
	time_t t;
	
	#ifdef CONFIG_SNIFFING_TIME
		int sniffing_time = CONFIG_SNIFFING_TIME;
	#else
		int sniffing_time = 60; // Default to 60 seconds
	#endif

	time(&t);
	st = (sniffing_time - (int)t % sniffing_time) * 1000;

	return st;
}

static void sniffer_task(void *pvParameter)
{
	#ifdef CONFIG_SNIFFING_TIME
		int sleep_time = CONFIG_SNIFFING_TIME*1000;
	#else
		int sleep_time = 60*1000; // Default to 60 seconds
	#endif

	ESP_LOGI(TAG, "[SNIFFER] Sniffer task created");

	// Wait for WiFi to be connected before starting sniffer
	ESP_LOGI(TAG, "[SNIFFER] Waiting for WiFi connection...");
	while(!WIFI_CONNECTED) {
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
	
	// Additional delay to ensure WiFi is stable
	vTaskDelay(2000 / portTICK_PERIOD_MS);

	ESP_LOGI(TAG, "[SNIFFER] Starting sniffing mode...");
	wifi_sniffer_init();
	
	#ifdef CONFIG_CHANNEL
		ESP_LOGI(TAG, "[SNIFFER] Started. Sniffing on channel %d", CONFIG_CHANNEL);
	#else
		ESP_LOGI(TAG, "[SNIFFER] Started. Sniffing on default channel");
	#endif

	while(true){
		vTaskDelay(sleep_time / portTICK_PERIOD_MS);
    	_lock_acquire(&lck_mqtt);
		/* if ESP is not connected to the broker
		 * -> need to reset file after 1 minutes, because wifi-task will not ever send it and initialize it */
		if(!MQTT_CONNECTED){
			ESP_LOGW(TAG, "[SNIFFER] Initializing file...");
			_lock_acquire(&lck_file);
			if(WHICH_FILE) {
				#ifdef CONFIG_FILENAME1
					file_init(CONFIG_FILENAME1);
				#else
					file_init("/spiffs/probreq.log");
				#endif
			} else {
				#ifdef CONFIG_FILENAME2
					file_init(CONFIG_FILENAME2);
				#else
					file_init("/spiffs/probreq2.log");
				#endif
			}
			_lock_release(&lck_file);
		}
    	_lock_release(&lck_mqtt);
	}
}

static void send_data()
{
	FILE *fp = NULL;
	int msg_id, tid;
	int sending = true, reading = true, tot_read = 0, n = 1;
	char *topic, buffer[BUFFSIZE], last_pkt = 'F';
	
	// Define fallback values for topic construction
	const char* ets_name = 
		#ifdef CONFIG_ETS
			CONFIG_ETS;
		#else
			"ETS";
		#endif
	
	const char* room_name = 
		#ifdef CONFIG_ROOM
			CONFIG_ROOM;
		#else
			"ROOM1";
		#endif
	
	const char* esp_id = 
		#ifdef CONFIG_ESP32_ID
			CONFIG_ESP32_ID;
		#else
			"ESP32_SNIFFER";
		#endif
	
	const char* filename1 = 
		#ifdef CONFIG_FILENAME1
			CONFIG_FILENAME1;
		#else
			"/spiffs/probreq.log";
		#endif
	
	const char* filename2 = 
		#ifdef CONFIG_FILENAME2
			CONFIG_FILENAME2;
		#else
			"/spiffs/probreq2.log";
		#endif
	
	ssize_t len = strlen(ets_name) + strlen(room_name) + strlen(esp_id) + 3; // +3 for two '/' and null terminator

	_lock_acquire(&lck_file);
	if(WHICH_FILE){
		WHICH_FILE = false;
		FILE_CHANGED = true;
		fp = fopen(filename1, "r");
		if(fp == NULL){
			RUNNING = false;
			ESP_LOGE(TAG, "[WI-FI] Impossible to open file %s and read information", filename1);
			_lock_release(&lck_file);
			return;
		}
	}
	else{
		WHICH_FILE = true;
		FILE_CHANGED = true;
		fp = fopen(filename2, "r");
		if(fp == NULL){
			RUNNING = false;
			ESP_LOGE(TAG, "[WI-FI] Impossible to open file %s and read information", filename2);
			_lock_release(&lck_file);
			return;
		}
	}
	_lock_release(&lck_file);

	topic = malloc(len*sizeof(char));
	if(topic == NULL) {
		ESP_LOGE(TAG, "[WI-FI] Failed to allocate memory for topic");
		fclose(fp);
		return;
	}
	
	memset(topic, '\0', len);
	strcpy(topic, ets_name);
	strcat(topic, "/");
	strcat(topic, room_name);
	strcat(topic, "/");
	strcat(topic, esp_id);

	if(fscanf(fp, "%d", &tid) != 1) {
		ESP_LOGW(TAG, "[WI-FI] No timestamp found in file, using 0");
		tid = 0;
	}

	ESP_LOGI(TAG, "[WI-FI] Would send data via MQTT (currently disabled)");
	// TODO: Implement actual MQTT sending when MQTT is re-enabled

	_lock_acquire(&lck_file);
	if(WHICH_FILE){
		fclose(fp);
		file_init((char*)filename2);
	}
	else{
		fclose(fp);
		file_init((char*)filename1);
	}
	_lock_release(&lck_file);

	free(topic);
}

static void wifi_sniffer_init()
{
	// Don't reinitialize WiFi - just set up promiscuous mode on existing connection
	ESP_LOGI(TAG, "[SNIFFER] Enabling promiscuous mode on existing WiFi connection...");
	
	esp_err_t ret;
	
	// Set the channel first
	#ifdef CONFIG_CHANNEL
		ret = esp_wifi_set_channel(CONFIG_CHANNEL, WIFI_SECOND_CHAN_NONE);
		if (ret != ESP_OK) {
			ESP_LOGW(TAG, "[SNIFFER] Failed to set channel %d: %s", CONFIG_CHANNEL, esp_err_to_name(ret));
		}
	#else
		ESP_LOGI(TAG, "[SNIFFER] Using current WiFi channel");
	#endif

	// Set up promiscuous filter
	const wifi_promiscuous_filter_t filt = {
			.filter_mask = WIFI_EVENT_MASK_AP_PROBEREQRECVED
	};
	ret = esp_wifi_set_promiscuous_filter(&filt);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "[SNIFFER] Failed to set promiscuous filter: %s", esp_err_to_name(ret));
		return;
	}
	
	// Set promiscuous callback
	ret = esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_packet_handler);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "[SNIFFER] Failed to set promiscuous callback: %s", esp_err_to_name(ret));
		return;
	}
	
	// Enable promiscuous mode
	ret = esp_wifi_set_promiscuous(true);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "[SNIFFER] Failed to enable promiscuous mode: %s", esp_err_to_name(ret));
		return;
	}
	
	ESP_LOGI(TAG, "[SNIFFER] Promiscuous mode enabled successfully");
}

static void wifi_sniffer_deinit()
{
	ESP_LOGI(TAG, "[SNIFFER] Disabling promiscuous mode...");
	esp_err_t ret = esp_wifi_set_promiscuous(false);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "[SNIFFER] Failed to disable promiscuous mode: %s", esp_err_to_name(ret));
	}
	// Don't deinit WiFi completely since we need it for MQTT
}

static void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type)
{
	int pkt_len, fc, sn=0;
	char ssid[SSID_MAX_LEN] = "\0", hash[MD5_LEN] = "\0", htci[5] = "\0";
	uint8_t ssid_len;
	time_t ts;

	wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buff;
	wifi_mgmt_hdr *mgmt = (wifi_mgmt_hdr *)pkt->payload;

	fc = ntohs(mgmt->fctl);

	if((fc & 0xFF00) == 0x4000){ //only look for probe request packets
		time(&ts);

		ssid_len = pkt->payload[25];
		if(ssid_len > 0)
			get_ssid(pkt->payload, ssid, ssid_len);

		pkt_len = pkt->rx_ctrl.sig_len;
		get_hash(pkt->payload, pkt_len-4, hash);

		if(CONFIG_VERBOSE){
			ESP_LOGI(TAG, "Dump");
			dumb(pkt->payload, pkt_len);
		}

		sn = get_sn(pkt->payload);

		get_ht_capabilites_info(pkt->payload, htci, pkt_len, ssid_len);

		ESP_LOGI(TAG, "ADDR=%02x:%02x:%02x:%02x:%02x:%02x, "
				"SSID=%s, "
				"TIMESTAMP=%d, "
				"HASH=%s, "
				"RSSI=%02d, "
				"SN=%d, "
				"HT CAP. INFO=%s",
				mgmt->sa[0], mgmt->sa[1], mgmt->sa[2], mgmt->sa[3], mgmt->sa[4], mgmt->sa[5],
				ssid,
				(int)ts,
				hash,
				pkt->rx_ctrl.rssi,
				sn,
				htci);

		save_pkt_info(mgmt->sa, ssid, ts, hash, pkt->rx_ctrl.rssi, sn, htci);
	}
}

static void get_hash(unsigned char *data, int len_res, char hash[MD5_LEN])
{
	uint8_t pkt_hash[16];

	md5((uint8_t *)data, len_res, pkt_hash);

	sprintf(hash, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
			pkt_hash[0], pkt_hash[1], pkt_hash[2], pkt_hash[3], pkt_hash[4], pkt_hash[5],
			pkt_hash[6], pkt_hash[7], pkt_hash[8], pkt_hash[9], pkt_hash[10], pkt_hash[11],
			pkt_hash[12], pkt_hash[13], pkt_hash[14], pkt_hash[15]);
}

static void get_ssid(unsigned char *data, char ssid[SSID_MAX_LEN], uint8_t ssid_len)
{
	int i, j;

	for(i=26, j=0; i<26+ssid_len; i++, j++){
		ssid[j] = data[i];
	}

	ssid[j] = '\0';
}

static int get_sn(unsigned char *data)
{
	int sn;
    char num[5] = "\0";

	sprintf(num, "%02x%02x", data[22], data[23]);
    sscanf(num, "%x", &sn);

    return sn;
}

static void get_ht_capabilites_info(unsigned char *data, char htci[5], int pkt_len, int ssid_len)
{
	int ht_start = 25+ssid_len+19;

	/* 1) data[ht_start-1] is the byte that says if HT Capabilities is present or not (tag length).
	 * 2) I need to check also that i'm not outside the payload: if HT Capabilities is not present in the packet,
	 * for this reason i'm considering the ht_start must be lower than the total length of the packet less the last 4 bytes of FCS */

	if(data[ht_start-1]>0 && ht_start<pkt_len-4){ //HT capabilities is present
		if(data[ht_start-4] == 1) //DSSS parameter is set -> need to shift of three bytes
			sprintf(htci, "%02x%02x", data[ht_start+3], data[ht_start+1+3]);
		else
			sprintf(htci, "%02x%02x", data[ht_start], data[ht_start+1]);
	}
}

static void dumb(unsigned char *data, int len)
{
	unsigned char i, j, byte;

	for(i=0; i<len; i++){
		byte = data[i];
		printf("%02x ", data[i]);

		if(((i%16)==15) || (i==len-1)){
			for(j=0; j<15-(i%16); j++)
				printf(" ");
			printf("| ");
			for(j=(i-(i%16)); j<=i; j++){
				byte = data[j];
				if((byte>31) && (byte<127))
					printf("%c", byte);
				else
					printf(".");
			}
			printf("\n");
		}
	}
}

static void save_pkt_info(uint8_t address[6], char *ssid, time_t timestamp, char *hash, int8_t rssi, int sn, char htci[5])
{
	FILE *fp = NULL;
	int stime;
	
	const char* filename1 = 
		#ifdef CONFIG_FILENAME1
			CONFIG_FILENAME1;
		#else
			"/spiffs/probreq.log";
		#endif
	
	const char* filename2 = 
		#ifdef CONFIG_FILENAME2
			CONFIG_FILENAME2;
		#else
			"/spiffs/probreq2.log";
		#endif

	_lock_acquire(&lck_file);
	if(WHICH_FILE)
		fp = fopen(filename1, "a");
	else
		fp = fopen(filename2, "a");
	_lock_release(&lck_file);

	if(fp == NULL){
		ESP_LOGE(TAG, "[SNIFFER] Impossible to open file and save information about sniffed packets");
		return;
	}

	if(FILE_CHANGED){
		FILE_CHANGED = false;
		stime = get_start_timestamp();
		fprintf(fp, "%d\n", stime);
	}

	fprintf(fp, "%02x:%02x:%02x:%02x:%02x:%02x %s %d %s %02d %d %s\n",
			address[0], address[1], address[2], address[3], address[4], address[5],
			ssid,
			(int)timestamp,
			hash,
			rssi,
			sn,
			htci);

	fclose(fp);
}

static int get_start_timestamp()
{
	int stime;
	time_t clk;
	
	#ifdef CONFIG_SNIFFING_TIME
		int sniffing_time = CONFIG_SNIFFING_TIME;
	#else
		int sniffing_time = 60; // Default to 60 seconds
	#endif

	time(&clk);
	stime = (int)clk - (int)clk % sniffing_time;

	return stime;
}

static void reboot(char *msg_err)
{
	int i;

	ESP_LOGE(TAG, "%s", msg_err);
    for(i=3; i>=0; i--){
        ESP_LOGW(TAG, "Restarting in %d seconds...", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    ESP_LOGW(TAG, "Restarting now");
    fflush(stdout);

    esp_restart();
}