#define ST7796_DRIVER
#define TFT_WIDTH 320
#define TFT_HEIGHT 480
#define TFT_RGB_ORDER TFT_RGB  // Revert to RGB
#define TFT_INVERSION_OFF      // Revert to non-inverted
#define TFT_MISO 12
#define TFT_MOSI 23  // Try alternative
#define TFT_SCLK 18  // Try alternative
#define TFT_CS   5   // Try alternative
#define TFT_DC   17  // Try alternative
#define TFT_RST  -1
#define TFT_BL   27
#define ESP32_DMA
#define USE_HSPI_PORT
#define SPI_FREQUENCY 20000000  // Reduced to 20MHz