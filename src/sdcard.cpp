
#include "sdcard.h"
#include "recording_helpers.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstring>

static std::atomic<bool> pending_toggle{false};
static std::atomic<bool> recording_transition{false};
static SemaphoreHandle_t recording_mutex;
static bool is_recording;
static uint8_t led_blink_type; // 0 - idle, do nothing, 1 - slow, 2 - fast, 10 - on, others - off
static File new_file_rx;
static File new_file_tx;
static char rand_folder_name[10];
static bool is_first_check = true;

/**
 * codes from:
 * https://github.com/espressif/arduino-esp32/blob/master/libraries/SD_MMC/examples/SDMMC_Test/SDMMC_Test.ino

void listDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
  Serial.printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root)
  {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if (levels)
      {
        listDir(fs, file.path(), levels - 1);
      }
    }
    else
    {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

void createDir(fs::FS &fs, const char *path)
{
  Serial.printf("Creating Dir: %s\n", path);
  if (fs.mkdir(path))
  {
    Serial.println("Dir created");
  }
  else
  {
    Serial.println("mkdir failed");
  }
}

void removeDir(fs::FS &fs, const char *path)
{
  Serial.printf("Removing Dir: %s\n", path);
  if (fs.rmdir(path))
  {
    Serial.println("Dir removed");
  }
  else
  {
    Serial.println("rmdir failed");
  }
}

void readFile(fs::FS &fs, const char *path)
{
  Serial.printf("Reading file: %s\n", path);

  File file = fs.open(path);
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return;
  }

  Serial.print("Read from file: ");
  while (file.available())
  {
    Serial.write(file.read());
  }
}

void writeFile(fs::FS &fs, const char *path, const char *message)
{
  Serial.printf("Writing file: %s\n", path);

  File file = fs.open(path, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }
  if (file.print(message))
  {
    Serial.println("File written");
  }
  else
  {
    Serial.println("Write failed");
  }
}

void appendFile(fs::FS &fs, const char *path, const char *message)
{
  Serial.printf("Appending to file: %s\n", path);

  File file = fs.open(path, FILE_APPEND);
  if (!file)
  {
    Serial.println("Failed to open file for appending");
    return;
  }
  if (file.print(message))
  {
    Serial.println("Message appended");
  }
  else
  {
    Serial.println("Append failed");
  }
}

void renameFile(fs::FS &fs, const char *path1, const char *path2)
{
  Serial.printf("Renaming file %s to %s\n", path1, path2);
  if (fs.rename(path1, path2))
  {
    Serial.println("File renamed");
  }
  else
  {
    Serial.println("Rename failed");
  }
}

void deleteFile(fs::FS &fs, const char *path)
{
  Serial.printf("Deleting file: %s\n", path);
  if (fs.remove(path))
  {
    Serial.println("File deleted");
  }
  else
  {
    Serial.println("Delete failed");
  }
}

void testFileIO(fs::FS &fs, const char *path)
{
  File file = fs.open(path);
  static uint8_t buf[512];
  size_t len = 0;
  uint32_t start = millis();
  uint32_t end = start;
  if (file)
  {
    len = file.size();
    size_t flen = len;
    start = millis();
    while (len)
    {
      size_t toRead = len;
      if (toRead > 512)
      {
        toRead = 512;
      }
      file.read(buf, toRead);
      len -= toRead;
    }
    end = millis() - start;
    Serial.printf("%u bytes read for %u ms\n", flen, end);
    file.close();
  }
  else
  {
    Serial.println("Failed to open file for reading");
  }

  file = fs.open(path, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }

  size_t i;
  start = millis();
  for (i = 0; i < 2048; i++)
  {
    file.write(buf, 512);
  }
  end = millis() - start;
  Serial.printf("%u bytes written for %u ms\n", 2048 * 512, end);
  file.close();
}
 */

// Caller holds recording_mutex so files cannot be closed during a write.
static void rec_stop()
{
  is_recording = false;
  new_file_rx.close();
  new_file_tx.close();
  SD_MMC.end();
  led_blink_type = 99;
  log_i("recording stopped");
}

void sdcard_toggle_rec(void)
{
  if (recording_mutex != nullptr)
    pending_toggle.store(true);
}

void rec(bool is_rx, char *s)
{
  if (recording_mutex == nullptr || recording_transition.load() || s == nullptr)
    return;
  xSemaphoreTake(recording_mutex, portMAX_DELAY);
  if (!is_recording || recording_transition.load())
  {
    xSemaphoreGive(recording_mutex);
    return;
  }
  hold_power_on = true;
  if (digitalRead(PIN_SD_CD) == LOW && !batt_low)
  {
    File &file = is_rx ? new_file_rx : new_file_tx;
    const size_t expected = std::strlen(s);
    if (!file || file.print(s) != expected)
    {
      log_e("recording write failed or was incomplete");
      rec_stop();
    }
  }
  else
  {
    log_w("SD removed or battery low during recording");
    rec_stop();
  }
  hold_power_on = false;
  xSemaphoreGive(recording_mutex);
}

static void task_led_blink(void *pvParameters)
{
  for (;;)
  {
    switch (led_blink_type)
    {
    case 0:
      // idle
      break;

    case 1:
      // slow
      digitalWrite(PIN_LED_REC, HIGH);
      vTaskDelay(200);
      digitalWrite(PIN_LED_REC, LOW);
      vTaskDelay(200);
      digitalWrite(PIN_LED_REC, HIGH);
      vTaskDelay(200);
      digitalWrite(PIN_LED_REC, LOW);
      led_blink_type = 0;
      break;

    case 2:
      // fast
      digitalWrite(PIN_LED_REC, HIGH);
      vTaskDelay(100);
      digitalWrite(PIN_LED_REC, LOW);
      vTaskDelay(100);
      digitalWrite(PIN_LED_REC, HIGH);
      vTaskDelay(100);
      digitalWrite(PIN_LED_REC, LOW);
      led_blink_type = 0;
      break;

    case 10:
      // always on
      digitalWrite(PIN_LED_REC, HIGH);
      led_blink_type = 0;
      break;

    default:
      // off
      digitalWrite(PIN_LED_REC, LOW);
      led_blink_type = 0;
      break;
    }
    vTaskDelay(100);
  }
  vTaskDelete(NULL);
}

static bool prepare_recording_folder()
{
  if (is_first_check)
  {
    if (!create_recording_folder(SD_MMC, rand_folder_name, sizeof(rand_folder_name),
                                 CREATE_MAX_RETRY, []() { return random(10000); }))
    {
      log_e("failed to create recording directory after %u attempts", CREATE_MAX_RETRY);
      return false;
    }
    is_first_check = false;
    log_i("folder created: %s", rand_folder_name);
    return true;
  }
  if (!SD_MMC.exists(rand_folder_name))
  {
    if (!SD_MMC.mkdir(rand_folder_name))
    {
      log_e("failed to recreate recording directory: %s", rand_folder_name);
      return false;
    }
    return true;
  }
  File directory = SD_MMC.open(rand_folder_name);
  const bool valid = directory && directory.isDirectory();
  directory.close();
  if (!valid)
    log_e("recording path is not a directory: %s", rand_folder_name);
  return valid;
}

static bool rec_start(uint8_t &error_blink)
{
  error_blink = 2;
  if (digitalRead(PIN_SD_CD) != LOW || batt_low)
  {
    error_blink = 1;
    log_w("no SD card detected or battery low");
    return false;
  }
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT))
  {
    log_e("SD card mount failed");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE)
  {
    log_e("no SD card attached");
    return false;
  }
  if (!prepare_recording_folder())
    return false;

  char path_rx[30];
  char path_tx[30];
  const unsigned long timestamp = millis();
  snprintf(path_rx, sizeof(path_rx), "%s/RX_%lu.txt", rand_folder_name, timestamp);
  snprintf(path_tx, sizeof(path_tx), "%s/TX_%lu.txt", rand_folder_name, timestamp);
  new_file_rx = SD_MMC.open(path_rx, FILE_APPEND);
  new_file_tx = SD_MMC.open(path_tx, FILE_APPEND);
  if (!new_file_rx || !new_file_tx)
  {
    log_e("failed to open both recording files");
    return false;
  }
  // Publish recording status only once the directory and both files are ready.
  is_recording = true;
  led_blink_type = 10;
  log_i("recording RX=%s TX=%s", path_rx, path_tx);
  return true;
}

static void task_rec_status(void *pvParameters)
{
  for (;;)
  {
    if (pending_toggle.exchange(false))
    {
      recording_transition.store(true);
      xSemaphoreTake(recording_mutex, portMAX_DELAY);
      hold_power_on = true;
      if (is_recording)
        rec_stop();
      else
      {
        uint8_t error_blink;
        if (!rec_start(error_blink))
        {
          rec_stop();
          led_blink_type = error_blink;
        }
      }
      hold_power_on = false;
      xSemaphoreGive(recording_mutex);
      recording_transition.store(false);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void sdcard_init(void)
{
  recording_mutex = xSemaphoreCreateMutex();
  if (recording_mutex == nullptr)
  {
    log_e("failed to allocate recording mutex");
    return;
  }
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_DATA0);
  if (xTaskCreatePinnedToCore(task_rec_status, "rec status chkr", 8192, nullptr, 1, nullptr, 0) != pdPASS)
  {
    log_e("failed to create recording task");
    vSemaphoreDelete(recording_mutex);
    recording_mutex = nullptr;
    return;
  }
  if (xTaskCreatePinnedToCore(task_led_blink, "led rec", 2048, nullptr, 1, nullptr, 0) != pdPASS)
    log_e("failed to create recording LED task");
}
