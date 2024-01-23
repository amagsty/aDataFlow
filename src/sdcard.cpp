
#include "sdcard.h"
bool is_rec_status_changing;
static bool is_recording;
static uint8_t led_blink_type; // 0 - idle, do nothing, 1 - slow, 2 - fast, 10 - on, others - off
static File new_file;
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

void rec_stop()
{
  log_i("stop recording...");
  new_file.close();
  SD_MMC.end();
  led_blink_type = 99;
  is_recording = false;
  is_rec_status_changing = false;
  log_i("recording stopped");
}

void rec(String &s, const char *dir)
{
  if (is_rec_status_changing || is_recording == false)
  {
    return;
  }
  // hold power until written
  if (digitalRead(PIN_SD_CD) == LOW && batt_low == false)
  {
    if (new_file)
    {
      hold_power_on = true;
      // rec format:
      // 00000 TX content... \n
      if (new_file.printf("%lu\t%s\t%s\n", millis(), dir, s.c_str()))
      {
        log_d("rec %s: %s", dir, s.c_str());
      }
      else
      {
        rec_stop();
        log_e("rec error");
      }
      hold_power_on = false;
    }
    else
    {
      rec_stop();
      log_e("file error");
    }
  }
  else
  {
    log_i("sd card was removed...");
    rec_stop();
  }
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

static void task_rec_status(void *pvParameters)
{
  for (;;)
  {
    if (is_rec_status_changing)
    {
      if (is_recording)
      {
        // toggle to stop recording and unmount sdcard
        rec_stop();
      }
      else
      {
        // mount sdcard and toggle to start recording
        log_i("start recording...");
        if (digitalRead(PIN_SD_CD) == LOW && batt_low == false)
        {
          // sdcard exist
          if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT))
          {
            is_recording = false;
            led_blink_type = 2;
            log_e("Card Mount Failed");
          }
          else
          {
            uint8_t cardType = SD_MMC.cardType();
            if (cardType == CARD_NONE)
            {
              is_recording = false;
              led_blink_type = 2;
              log_i("No SD_MMC card attached");
            }
            else
            {
              log_i("SD_MMC Card Type: %d (1=MMC, 2=SDSC, 3=SDHC)", cardType);
              uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
              log_i("SD_MMC Card Size: %lluMB", cardSize);
              log_i("Total space: %lluMB", SD_MMC.totalBytes() / (1024 * 1024));
              log_i("Used space: %lluMB", SD_MMC.usedBytes() / (1024 * 1024));
              led_blink_type = 10;
              is_recording = true;

              // filename: DAT_rand(0-10000)/millis_from_boot.txt
              // DAT_9999/99999999.txt
              if (is_first_check)
              {
                // generate one new data folder:
                uint8_t chk_counter;
                for (size_t i = 0; i < CREATE_MAX_RETRY; i++)
                {
                  chk_counter++;
                  snprintf(rand_folder_name, sizeof(rand_folder_name), "/DAT_%04d", random(10000));
                  if (!SD_MMC.exists(rand_folder_name))
                  {
                    SD_MMC.mkdir(rand_folder_name);
                    log_i("folder created: %s", rand_folder_name);
                    break;
                  }
                }
                if (chk_counter >= CREATE_MAX_RETRY)
                {
                  log_e("no available folder name");
                  rec_stop();
                }
                else
                {
                  is_first_check = false;
                }
              }
              else
              {
                if (!SD_MMC.exists(rand_folder_name))
                {
                  SD_MMC.mkdir(rand_folder_name);
                  log_i("folder recreated: %s", rand_folder_name);
                }
              }

              char path[30];
              // /DAT_9999/4294967295.txt
              snprintf(path, sizeof(path), "%s/%ld.txt", rand_folder_name, millis());
              log_i("try this file: %s", path);
              new_file = SD_MMC.open(path, FILE_APPEND);
              if (!new_file)
              {
                log_e("failed to open file for saving");
                rec_stop();
              }
              else
              {
                log_i("rec to file: %s", path);
              }
            }
          }
        }
        else
        {
          // no sdcard
          // blink led for error
          led_blink_type = 1;
          is_recording = false;
          log_i("no sdcard detected");
        }
      }
      is_rec_status_changing = false;
    }
    vTaskDelay(100);
  }
  vTaskDelete(NULL);
}

void sdcard_init(void)
{
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_DATA0);
  xTaskCreatePinnedToCore(task_led_blink, "led rec", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(task_rec_status, "rec status chkr", 8192, NULL, 1, NULL, 0);
}