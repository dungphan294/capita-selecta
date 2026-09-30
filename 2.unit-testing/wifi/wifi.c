#include <stdio.h>
#include "pico/stdlib.h"     // Include the standard library header for input/output functions
#include "pico/cyw43_arch.h" // Include the header for the CYW43 architecture, which is used for Wi-Fi functionality
#include "lwip/netif.h"      // Include the header for the lwIP network interface functions

// WIFI_SSID and WIFI_PASSWORD are supplied by CMake from wifi_secrets.cmake

int main()
{
    stdio_init_all(); // Initialize standard input/output functions
    sleep_ms(3000); // Sleep for 3000 milliseconds (3 seconds) to allow time for initialization

    if (WIFI_SSID[0] == '\0') {
        printf("ERROR: WIFI_SSID is empty.\n");
        printf("Set it in wifi_secrets.cmake, then delete build/ and reconfigure.\n");
        while (true) { sleep_ms(1000); }
    }

    printf("Starting Wi-Fi...\n"); // Print a message indicating that Wi-Fi is starting

    if(cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS)) // Initialize the CYW43 architecture with the country code for the Netherlands
    {
        printf("Wi-Fi init failed\n"); // Print an error message if Wi-Fi initialization fails
        return 1; // Return 1 to indicate failure
    }

    cyw43_arch_enable_sta_mode(); // Enable station mode for Wi-Fi, allowing the device to connect to a Wi-Fi network

    printf("Connecting to '%s'\n", WIFI_SSID); // Print a message indicating that Wi-Fi has started
    int err = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_MIXED_PSK, 30000); // Attempt to connect to the specified Wi-Fi network with a timeout of 30 seconds
    
    if (err) // Check if there was an error during the connection attempt
    {
        printf("Failed to connect to Wi-Fi: %d\n", err); // Print an error message with the error code
        return 1; // Return 1 to indicate failure
    }

    printf("Connected to Wi-Fi\n"); // Print a message indicating that the device is connected to Wi-Fi
    printf("IP address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default))); // Print the IP address assigned to the device

    while (true)
    {
        // blink the LED to indicate that the program is running
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(500);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(500);
    }
}
