#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <AccelStepper.h>
#include <Wire.h>  // Only needed for Arduino 1.6.5 and earlier
#include "FS.h"
#ifndef PSTR
#define PSTR // Make Arduino Due happy
#endif
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include "ezTime.h"




/*********
  Wifi Config
********/
ESP8266WebServer    server(80);
struct settings {
  char ssid[32];
  char password[63];
} user_wifi = {};




/*********
  Timezone Config
********/
char mylocation[40];
Timezone myTZ;




/*********
  Stepper Config
********/
const int stepsPerRevolution = 2048;  // change this to fit the number of steps per revolution

// ULN2003 Motor Driver Pins
#define IN1 D1
#define IN2 D2
#define IN3 D3
#define IN4 D4

// initialize the stepper library
AccelStepper stepper(AccelStepper::HALF4WIRE, IN1, IN3, IN2, IN4);




/*********
  Default Variable Config
********/
#define Button1Pin D5
#define Button2Pin D6
#define HallSensorPin D7

int HallSensor = 0;
int Button1 = 0;
int Button2 = 0;
long nextmidnight = millis();
int wehavesavedwifidetails = 0;
int justpoweredon = 1;
int previousday = 9;
int currentday = 9;


/*******************
Change this to something unique, only used to access portal to add your wifi credentials and timezone
********************/
char defaultwifipassword[63] = "saturday";

/********************
  You will have change this, it depends when your hall sensor finds your magnet, and if Sunday aligns satisfactorily with the view window when it does.
  This depends on where you glued your sensor, so this value allows you to make sure the words are in the centre of the view port.
*******************/
int SundayOffset = -260;




bool saveConfig() {
  StaticJsonDocument<200> doc;

  doc["mylocation"] = mylocation;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return false;
  }

  serializeJson(doc, configFile);
  return true;
}




bool loadConfig() {
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(buf.get(), size);

  StaticJsonDocument<200> doc;
  auto error = deserializeJson(doc, buf.get());
  if (error) {
    Serial.println("Failed to parse config file");
    return false;
  }

  strncpy(mylocation, doc["mylocation"], sizeof(mylocation));

  Serial.print("Loaded config ");

  return true;
}




void setup() {
  // initialize serial
  Serial.begin(115200);

  // make the hall sensor pin and switches input, and pull them high
  pinMode(HallSensorPin, INPUT_PULLUP);
  pinMode(Button1Pin, INPUT_PULLUP);
  pinMode(Button2Pin, INPUT_PULLUP);

  stepper.setMaxSpeed(500);
  stepper.setAcceleration(100);

  //now + 24hours
  nextmidnight = millis() + 86400000;

  //for testing
  //nextmidnight = millis() + 10000;

  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }

  if (!loadConfig()) {
    Serial.println("Failed to load config");
    strcpy(mylocation, "Europe/London");
  } else {
    Serial.println("Config loaded");
  }

  //wifi config settings
  EEPROM.begin(sizeof(struct settings) );
  EEPROM.get( 0, user_wifi );

  if (user_wifi.ssid == "empty" || sizeof(user_wifi.ssid) <= 2) {
    wehavesavedwifidetails = 0;
  } else {
    wehavesavedwifidetails = 1;
  }

  if (wehavesavedwifidetails == 1) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(user_wifi.ssid, user_wifi.password);
    byte tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);
      if (tries++ > 30) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("DoW Clock Settings 192.168.4.1", defaultwifipassword);
        break;
      }
    }

    //get time
    setInterval(600);
    waitForSync(30);
    Serial.println();
    Serial.println("UTC:             " + UTC.dateTime());

    myTZ.setLocation(mylocation);
    Serial.print(F("Time in UK:     "));
    Serial.println(myTZ.dateTime());
  }

  if (wehavesavedwifidetails == 0) {
    WiFi.begin();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("DoW Clock Settings 192.168.4.1", defaultwifipassword);
  }

  server.on("/",  handlePortal);
  server.begin();
}




void advanceaday() {
  //Rotate to next day
  stepper.enableOutputs();
  stepper.setCurrentPosition(0);
  stepper.moveTo((585) * -1);

  while (stepper.distanceToGo() != 0) {
    stepper.run();
    delay(10);
  }
  stepper.disableOutputs();
}




void advancetosunday() {
  stepper.enableOutputs();

  int localSundayOffset = 0;

  if (HallSensor == LOW) {
    localSundayOffset = 0;
  } else {
    localSundayOffset = SundayOffset;
  }

  //Find sunday
  //Rotate until the magnet is found
  stepper.setCurrentPosition(0);
  stepper.moveTo(((stepsPerRevolution * 2) * -1));

  while (stepper.distanceToGo() != 0) {
    HallSensor = digitalRead(HallSensorPin);
    if (HallSensor == LOW) {
      stepper.setCurrentPosition(0);
    }
    stepper.run();
    delay(10);
  }

  //now apply the offset so Sunday lines up
  stepper.setCurrentPosition(0);
  stepper.moveTo(localSundayOffset * -1);

  while (stepper.distanceToGo() != 0) {
    stepper.run();
    delay(10);
  }

  // to take up rotational slack
  stepper.setCurrentPosition(0);
  stepper.moveTo(-100);
  while (stepper.distanceToGo() != 0) {
    stepper.run();
    delay(10);
  }

  stepper.disableOutputs();
}




void loop() {
  //keeps time synched
  events();
  server.handleClient();

  HallSensor = digitalRead(HallSensorPin);
  Button1 =  digitalRead(Button1Pin);
  Button2 =  digitalRead(Button2Pin);

  if (Button1 == LOW && Button2 == LOW) {
    //reset wifi ssid
    strncpy(user_wifi.ssid,     "empty",     sizeof(user_wifi.ssid) );
    strncpy(user_wifi.password, "empty", sizeof(user_wifi.password) );

    user_wifi.ssid[5] = user_wifi.password[5] = '\0';
    EEPROM.put(0, user_wifi);
    EEPROM.commit();

    Serial.println("******Wifi empty******");
  }

  if (Button1 == LOW) {
    //assuming a 12 noon last press
    nextmidnight = millis() + 43200000 - 5000;
    
    Serial.println ("Advance to Sunday pressed");
    advancetosunday();
  }


  if (Button2 == LOW) {
    Serial.println ("Day Advance Pressed");

    //assuming a 12 noon last press
    nextmidnight = millis() + 43200000 - 5000;

    //for testing
    //nextmidnight = millis() + 10000;

    advanceaday();
  }

  if (wehavesavedwifidetails == 0) {
    //Basic reboot at midnight mode
    //Check if it's midnight

    if (millis() > nextmidnight) {
      //move it a day and reboot to reset millis
      advanceaday();
      ESP.reset();
    }

    //for testing
    //Serial.println(millis());
    //Serial.println(nextmidnight);
    delay(1000);
  } else {
    //we use ezTime to check if the day has changed.

    if (justpoweredon == 1) {
      //we don't know what day it is, and the clock may be showing the wrong day
      currentday = weekday();
      previousday = weekday();

      //rotate the clock to Sunday, then advance as required to correct day.
      advancetosunday();

      //Should now be at Sunday (1)
      for (int i = 0; i < (currentday - 1); i++) {
        //advance to the current day
        advanceaday();
      }

      justpoweredon = 0;

    } else {
      currentday = weekday();

      //check if we should advance the day or not

      if (currentday != previousday) {

        //we've just passed midnight

        //if it's not Sunday
        if (currentday != 1) {
          advanceaday();
        } else {
          //advance to Sunday using the magnet - this prevents the days drifting out of the view window
          advancetosunday();
        }
        previousday = currentday;
      }
    }
  }

  //for debugging can be commented out
  //Serial.print("Hall Sensor = ");
  //Serial.println(HallSensor);
  //Serial.print("Week day = ");
  //Serial.println(weekday());
  //Serial.print("Saved wifi details = ");
  //Serial.println(wehavesavedwifidetails);
  //Serial.print("SSID = ");
  //Serial.println(user_wifi.ssid);

  delay(5000);
}




void handlePortal() {

  if (server.method() == HTTP_POST) {
    strncpy(user_wifi.ssid,     server.arg("ssid").c_str(),     sizeof(user_wifi.ssid) );
    strncpy(user_wifi.password, server.arg("password").c_str(), sizeof(user_wifi.password) );
    strncpy(mylocation, server.arg("location").c_str(), sizeof(mylocation) );

    user_wifi.ssid[server.arg("ssid").length()] = user_wifi.password[server.arg("password").length()] = '\0';
    EEPROM.put(0, user_wifi);
    EEPROM.commit();

    server.send(200,   "text/html",  "<!doctype html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Day of Week Clock Setup</title><style>*,::after,::before{box-sizing:border-box;}body{margin:0;font-family:'Segoe UI',Roboto,'Helvetica Neue',Arial,'Noto Sans','Liberation Sans';font-size:1rem;font-weight:400;line-height:1.5;color:#212529;background-color:#f5f5f5;}.form-control{display:block;width:100%;height:calc(1.5em + .75rem + 2px);border:1px solid #ced4da;}button{border:1px solid transparent;color:#fff;background-color:#007bff;border-color:#007bff;padding:.5rem 1rem;font-size:1.25rem;line-height:1.5;border-radius:.3rem;width:100%}.form-signin{width:100%;max-width:400px;padding:15px;margin:auto;}h1,p{text-align: center}</style> </head> <body><main class='form-signin'> <h1>Wifi Setup</h1> <br/> <p>Your settings have been saved successfully!<br />The clock will now reboot and set itself to the correct day.</p></main></body></html>");
    saveConfig();
    delay(1000);
    ESP.restart();

  } else {

    // server.send(200, "text/html", );
    server.send(200, "text/html", F("<!doctype html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Day of Week Clock Setup</title><style>*,::after,::before{box-sizing:border-box;}body{margin:0;font-family:'Segoe UI',Roboto,'Helvetica Neue',Arial,'Noto Sans','Liberation Sans';font-size:1rem;font-weight:400;line-height:1.5;color:#212529;background-color:#f5f5f5;}.form-control{display:block;width:100%;height:calc(1.5em + .75rem + 2px);border:1px solid #ced4da;}button{cursor: pointer;border:1px solid transparent;color:#fff;background-color:#007bff;border-color:#007bff;padding:.5rem 1rem;font-size:1.25rem;line-height:1.5;border-radius:.3rem;width:100%}.form-inline{input[type=radio] {display: inline;};;input, textarea,select {margin-top: 10px; display: block;}}.form-signin{width:100%;max-width:400px;padding:15px;margin:auto;}h1{text-align: center}</style></head><body><main class='form-signin'><form action='/' method='post'><h1 class=''>Clock Setup</h1><br/><div class='form-floating'><label>Enter your Wifi SSID</label><input type='text' class='form-control' name='ssid'></div><div class='form-floating'><br/><label>Enter your Wifi Password</label><input type='password' class='form-control' name='password'><br/><h4>Note this is not being sent by https or ssl</h4></div><br/><div class='form-inline'><label for='location'>Select your timezone</label><select name='location' id='location'><option value='Africa/Accra'>Africa/Accra</option><option value='Africa/Addis_Ababa'>Africa/Addis_Ababa</option><option value='Africa/Algiers'>Africa/Algiers</option><option value='Africa/Asmara'>Africa/Asmara</option><option value='Africa/Asmera'>Africa/Asmera</option><option value='Africa/Bamako'>Africa/Bamako</option><option value='Africa/Bangui'>Africa/Bangui</option><option value='Africa/Banjul'>Africa/Banjul</option><option value='Africa/Bissau'>Africa/Bissau</option><option value='Africa/Blantyre'>Africa/Blantyre</option><option value='Africa/Brazzaville'>Africa/Brazzaville</option><option value='Africa/Bujumbura'>Africa/Bujumbura</option><option value='Africa/Cairo'>Africa/Cairo</option><option value='Africa/Casablanca'>Africa/Casablanca</option><option value='Africa/Ceuta'>Africa/Ceuta</option><option value='Africa/Conakry'>Africa/Conakry</option><option value='Africa/Dakar'>Africa/Dakar</option><option value='Africa/Dar_es_Salaam'>Africa/Dar_es_Salaam</option><option value='Africa/Djibouti'>Africa/Djibouti</option><option value='Africa/Douala'>Africa/Douala</option><option value='Africa/El_Aaiun'>Africa/El_Aaiun</option><option value='Africa/Freetown'>Africa/Freetown</option><option value='Africa/Gaborone'>Africa/Gaborone</option><option value='Africa/Harare'>Africa/Harare</option><option value='Africa/Johannesburg'>Africa/Johannesburg</option><option value='Africa/Juba'>Africa/Juba</option><option value='Africa/Kampala'>Africa/Kampala</option><option value='Africa/Khartoum'>Africa/Khartoum</option><option value='Africa/Kigali'>Africa/Kigali</option><option value='Africa/Kinshasa'>Africa/Kinshasa</option><option value='Africa/Lagos'>Africa/Lagos</option><option value='Africa/Libreville'>Africa/Libreville</option><option value='Africa/Lome'>Africa/Lome</option><option value='Africa/Luanda'>Africa/Luanda</option><option value='Africa/Lubumbashi'>Africa/Lubumbashi</option><option value='Africa/Lusaka'>Africa/Lusaka</option><option value='Africa/Malabo'>Africa/Malabo</option><option value='Africa/Maputo'>Africa/Maputo</option><option value='Africa/Maseru'>Africa/Maseru</option><option value='Africa/Mbabane'>Africa/Mbabane</option><option value='Africa/Mogadishu'>Africa/Mogadishu</option><option value='Africa/Monrovia'>Africa/Monrovia</option><option value='Africa/Nairobi'>Africa/Nairobi</option><option value='Africa/Ndjamena'>Africa/Ndjamena</option><option value='Africa/Niamey'>Africa/Niamey</option><option value='Africa/Nouakchott'>Africa/Nouakchott</option><option value='Africa/Ouagadougou'>Africa/Ouagadougou</option><option value='Africa/Porto-Novo'>Africa/Porto-Novo</option><option value='Africa/Sao_Tome'>Africa/Sao_Tome</option><option value='Africa/Timbuktu'>Africa/Timbuktu</option><option value='Africa/Tripoli'>Africa/Tripoli</option><option value='Africa/Tunis'>Africa/Tunis</option><option value='Africa/Windhoek'>Africa/Windhoek</option><option value='America/Adak'>America/Adak</option><option value='America/Anchorage'>America/Anchorage</option><option value='America/Anguilla'>America/Anguilla</option><option value='America/Antigua'>America/Antigua</option><option value='America/Araguaina'>America/Araguaina</option><option value='America/Argentina/Buenos_Aires'>America/Argentina/Buenos_Aires</option><option value='America/Argentina/Catamarca'>America/Argentina/Catamarca</option><option value='America/Argentina/ComodRivadavia'>America/Argentina/ComodRivadavia</option><option value='America/Argentina/Cordoba'>America/Argentina/Cordoba</option><option value='America/Argentina/Jujuy'>America/Argentina/Jujuy</option><option value='America/Argentina/La_Rioja'>America/Argentina/La_Rioja</option><option value='America/Argentina/Mendoza'>America/Argentina/Mendoza</option><option value='America/Argentina/Rio_Gallegos'>America/Argentina/Rio_Gallegos</option><option value='America/Argentina/Salta'>America/Argentina/Salta</option><option value='America/Argentina/San_Juan'>America/Argentina/San_Juan</option><option value='America/Argentina/San_Luis'>America/Argentina/San_Luis</option><option value='America/Argentina/Tucuman'>America/Argentina/Tucuman</option><option value='America/Argentina/Ushuaia'>America/Argentina/Ushuaia</option><option value='America/Aruba'>America/Aruba</option><option value='America/Asuncion'>America/Asuncion</option><option value='America/Atikokan'>America/Atikokan</option><option value='America/Atka'>America/Atka</option><option value='America/Bahia'>America/Bahia</option><option value='America/Bahia_Banderas'>America/Bahia_Banderas</option><option value='America/Barbados'>America/Barbados</option><option value='America/Belem'>America/Belem</option><option value='America/Belize'>America/Belize</option><option value='America/Blanc-Sablon'>America/Blanc-Sablon</option><option value='America/Boa_Vista'>America/Boa_Vista</option><option value='America/Bogota'>America/Bogota</option><option value='America/Boise'>America/Boise</option><option value='America/Buenos_Aires'>America/Buenos_Aires</option><option value='America/Cambridge_Bay'>America/Cambridge_Bay</option><option value='America/Campo_Grande'>America/Campo_Grande</option><option value='America/Cancun'>America/Cancun</option><option value='America/Caracas'>America/Caracas</option><option value='America/Catamarca'>America/Catamarca</option><option value='America/Cayenne'>America/Cayenne</option><option value='America/Cayman'>America/Cayman</option><option value='America/Chicago'>America/Chicago</option><option value='America/Chihuahua'>America/Chihuahua</option><option value='America/Coral_Harbour'>America/Coral_Harbour</option><option value='America/Cordoba'>America/Cordoba</option><option value='America/Costa_Rica'>America/Costa_Rica</option><option value='America/Creston'>America/Creston</option><option value='America/Cuiaba'>America/Cuiaba</option><option value='America/Curacao'>America/Curacao</option><option value='America/Danmarkshavn'>America/Danmarkshavn</option><option value='America/Dawson'>America/Dawson</option><option value='America/Dawson_Creek'>America/Dawson_Creek</option><option value='America/Denver'>America/Denver</option><option value='America/Detroit'>America/Detroit</option><option value='America/Dominica'>America/Dominica</option><option value='America/Edmonton'>America/Edmonton</option><option value='America/Eirunepe'>America/Eirunepe</option><option value='America/El_Salvador'>America/El_Salvador</option><option value='America/Ensenada'>America/Ensenada</option><option value='America/Fort_Nelson'>America/Fort_Nelson</option><option value='America/Fort_Wayne'>America/Fort_Wayne</option><option value='America/Fortaleza'>America/Fortaleza</option><option value='America/Glace_Bay'>America/Glace_Bay</option><option value='America/Godthab'>America/Godthab</option><option value='America/Goose_Bay'>America/Goose_Bay</option><option value='America/Grand_Turk'>America/Grand_Turk</option><option value='America/Grenada'>America/Grenada</option><option value='America/Guadeloupe'>America/Guadeloupe</option><option value='America/Guatemala'>America/Guatemala</option><option value='America/Guayaquil'>America/Guayaquil</option><option value='America/Guyana'>America/Guyana</option><option value='America/Halifax'>America/Halifax</option><option value='America/Havana'>America/Havana</option><option value='America/Hermosillo'>America/Hermosillo</option><option value='America/Indiana/Indianapolis'>America/Indiana/Indianapolis</option><option value='America/Indiana/Knox'>America/Indiana/Knox</option><option value='America/Indiana/Marengo'>America/Indiana/Marengo</option><option value='America/Indiana/Petersburg'>America/Indiana/Petersburg</option><option value='America/Indiana/Tell_City'>America/Indiana/Tell_City</option><option value='America/Indiana/Vevay'>America/Indiana/Vevay</option><option value='America/Indiana/Vincennes'>America/Indiana/Vincennes</option><option value='America/Indiana/Winamac'>America/Indiana/Winamac</option><option value='America/Indianapolis'>America/Indianapolis</option><option value='America/Inuvik'>America/Inuvik</option><option value='America/Iqaluit'>America/Iqaluit</option><option value='America/Jamaica'>America/Jamaica</option><option value='America/Jujuy'>America/Jujuy</option><option value='America/Juneau'>America/Juneau</option><option value='America/Kentucky/Louisville'>America/Kentucky/Louisville</option><option value='America/Kentucky/Monticello'>America/Kentucky/Monticello</option><option value='America/Knox_IN'>America/Knox_IN</option><option value='America/Kralendijk'>America/Kralendijk</option><option value='America/La_Paz'>America/La_Paz</option><option value='America/Lima'>America/Lima</option><option value='America/Los_Angeles'>America/Los_Angeles</option><option value='America/Louisville'>America/Louisville</option><option value='America/Lower_Princes'>America/Lower_Princes</option><option value='America/Maceio'>America/Maceio</option><option value='America/Managua'>America/Managua</option><option value='America/Manaus'>America/Manaus</option><option value='America/Marigot'>America/Marigot</option><option value='America/Martinique'>America/Martinique</option><option value='America/Matamoros'>America/Matamoros</option><option value='America/Mazatlan'>America/Mazatlan</option><option value='America/Mendoza'>America/Mendoza</option><option value='America/Menominee'>America/Menominee</option><option value='America/Merida'>America/Merida</option><option value='America/Metlakatla'>America/Metlakatla</option><option value='America/Mexico_City'>America/Mexico_City</option><option value='America/Miquelon'>America/Miquelon</option><option value='America/Moncton'>America/Moncton</option><option value='America/Monterrey'>America/Monterrey</option><option value='America/Montevideo'>America/Montevideo</option><option value='America/Montreal'>America/Montreal</option><option value='America/Montserrat'>America/Montserrat</option><option value='America/Nassau'>America/Nassau</option><option value='America/New_York'>America/New_York</option><option value='America/Nipigon'>America/Nipigon</option><option value='America/Nome'>America/Nome</option><option value='America/Noronha'>America/Noronha</option><option value='America/North_Dakota/Beulah'>America/North_Dakota/Beulah</option><option value='America/North_Dakota/Center'>America/North_Dakota/Center</option><option value='America/North_Dakota/New_Salem'>America/North_Dakota/New_Salem</option><option value='America/Nuuk'>America/Nuuk</option><option value='America/Ojinaga'>America/Ojinaga</option><option value='America/Panama'>America/Panama</option><option value='America/Pangnirtung'>America/Pangnirtung</option><option value='America/Paramaribo'>America/Paramaribo</option><option value='America/Phoenix'>America/Phoenix</option><option value='America/Port-au-Prince'>America/Port-au-Prince</option><option value='America/Port_of_Spain'>America/Port_of_Spain</option><option value='America/Porto_Acre'>America/Porto_Acre</option><option value='America/Porto_Velho'>America/Porto_Velho</option><option value='America/Puerto_Rico'>America/Puerto_Rico</option><option value='America/Punta_Arenas'>America/Punta_Arenas</option><option value='America/Rainy_River'>America/Rainy_River</option><option value='America/Rankin_Inlet'>America/Rankin_Inlet</option><option value='America/Recife'>America/Recife</option><option value='America/Regina'>America/Regina</option><option value='America/Resolute'>America/Resolute</option><option value='America/Rio_Branco'>America/Rio_Branco</option><option value='America/Rosario'>America/Rosario</option><option value='America/Santa_Isabel'>America/Santa_Isabel</option><option value='America/Santarem'>America/Santarem</option><option value='America/Santiago'>America/Santiago</option><option value='America/Santo_Domingo'>America/Santo_Domingo</option><option value='America/Sao_Paulo'>America/Sao_Paulo</option><option value='America/Scoresbysund'>America/Scoresbysund</option><option value='America/Shiprock'>America/Shiprock</option><option value='America/Sitka'>America/Sitka</option><option value='America/St_Barthelemy'>America/St_Barthelemy</option><option value='America/St_Johns'>America/St_Johns</option><option value='America/St_Kitts'>America/St_Kitts</option><option value='America/St_Lucia'>America/St_Lucia</option><option value='America/St_Thomas'>America/St_Thomas</option><option value='America/St_Vincent'>America/St_Vincent</option><option value='America/Swift_Current'>America/Swift_Current</option><option value='America/Tegucigalpa'>America/Tegucigalpa</option><option value='America/Thule'>America/Thule</option><option value='America/Thunder_Bay'>America/Thunder_Bay</option><option value='America/Tijuana'>America/Tijuana</option><option value='America/Toronto'>America/Toronto</option><option value='America/Tortola'>America/Tortola</option><option value='America/Vancouver'>America/Vancouver</option><option value='America/Virgin'>America/Virgin</option><option value='America/Whitehorse'>America/Whitehorse</option><option value='America/Winnipeg'>America/Winnipeg</option><option value='America/Yakutat'>America/Yakutat</option><option value='America/Yellowknife'>America/Yellowknife</option><option value='Antarctica/Casey'>Antarctica/Casey</option><option value='Antarctica/Davis'>Antarctica/Davis</option><option value='Antarctica/DumontDUrville'>Antarctica/DumontDUrville</option><option value='Antarctica/Macquarie'>Antarctica/Macquarie</option><option value='Antarctica/Mawson'>Antarctica/Mawson</option><option value='Antarctica/McMurdo'>Antarctica/McMurdo</option><option value='Antarctica/Palmer'>Antarctica/Palmer</option><option value='Antarctica/Rothera'>Antarctica/Rothera</option><option value='Antarctica/South_Pole'>Antarctica/South_Pole</option><option value='Antarctica/Syowa'>Antarctica/Syowa</option><option value='Antarctica/Troll'>Antarctica/Troll</option><option value='Antarctica/Vostok'>Antarctica/Vostok</option><option value='Arctic/Longyearbyen'>Arctic/Longyearbyen</option><option value='Asia/Aden'>Asia/Aden</option><option value='Asia/Almaty'>Asia/Almaty</option><option value='Asia/Amman'>Asia/Amman</option><option value='Asia/Anadyr'>Asia/Anadyr</option><option value='Asia/Aqtau'>Asia/Aqtau</option><option value='Asia/Aqtobe'>Asia/Aqtobe</option><option value='Asia/Ashgabat'>Asia/Ashgabat</option><option value='Asia/Ashkhabad'>Asia/Ashkhabad</option><option value='Asia/Atyrau'>Asia/Atyrau</option><option value='Asia/Baghdad'>Asia/Baghdad</option><option value='Asia/Bahrain'>Asia/Bahrain</option><option value='Asia/Baku'>Asia/Baku</option><option value='Asia/Bangkok'>Asia/Bangkok</option><option value='Asia/Barnaul'>Asia/Barnaul</option><option value='Asia/Beirut'>Asia/Beirut</option><option value='Asia/Bishkek'>Asia/Bishkek</option><option value='Asia/Brunei'>Asia/Brunei</option><option value='Asia/Calcutta'>Asia/Calcutta</option><option value='Asia/Chita'>Asia/Chita</option><option value='Asia/Choibalsan'>Asia/Choibalsan</option><option value='Asia/Chongqing'>Asia/Chongqing</option><option value='Asia/Chungking'>Asia/Chungking</option><option value='Asia/Colombo'>Asia/Colombo</option><option value='Asia/Dacca'>Asia/Dacca</option><option value='Asia/Damascus'>Asia/Damascus</option><option value='Asia/Dhaka'>Asia/Dhaka</option><option value='Asia/Dili'>Asia/Dili</option><option value='Asia/Dubai'>Asia/Dubai</option><option value='Asia/Dushanbe'>Asia/Dushanbe</option><option value='Asia/Famagusta'>Asia/Famagusta</option><option value='Asia/Gaza'>Asia/Gaza</option><option value='Asia/Harbin'>Asia/Harbin</option><option value='Asia/Hebron'>Asia/Hebron</option><option value='Asia/Ho_Chi_Minh'>Asia/Ho_Chi_Minh</option><option value='Asia/Hong_Kong'>Asia/Hong_Kong</option><option value='Asia/Hovd'>Asia/Hovd</option><option value='Asia/Irkutsk'>Asia/Irkutsk</option><option value='Asia/Istanbul'>Asia/Istanbul</option><option value='Asia/Jakarta'>Asia/Jakarta</option><option value='Asia/Jayapura'>Asia/Jayapura</option><option value='Asia/Jerusalem'>Asia/Jerusalem</option><option value='Asia/Kabul'>Asia/Kabul</option><option value='Asia/Kamchatka'>Asia/Kamchatka</option><option value='Asia/Karachi'>Asia/Karachi</option><option value='Asia/Kashgar'>Asia/Kashgar</option><option value='Asia/Kathmandu'>Asia/Kathmandu</option><option value='Asia/Katmandu'>Asia/Katmandu</option><option value='Asia/Khandyga'>Asia/Khandyga</option><option value='Asia/Kolkata'>Asia/Kolkata</option><option value='Asia/Krasnoyarsk'>Asia/Krasnoyarsk</option><option value='Asia/Kuala_Lumpur'>Asia/Kuala_Lumpur</option><option value='Asia/Kuching'>Asia/Kuching</option><option value='Asia/Kuwait'>Asia/Kuwait</option><option value='Asia/Macao'>Asia/Macao</option><option value='Asia/Macau'>Asia/Macau</option><option value='Asia/Magadan'>Asia/Magadan</option><option value='Asia/Makassar'>Asia/Makassar</option><option value='Asia/Manila'>Asia/Manila</option><option value='Asia/Muscat'>Asia/Muscat</option><option value='Asia/Nicosia'>Asia/Nicosia</option><option value='Asia/Novokuznetsk'>Asia/Novokuznetsk</option><option value='Asia/Novosibirsk'>Asia/Novosibirsk</option><option value='Asia/Omsk'>Asia/Omsk</option><option value='Asia/Oral'>Asia/Oral</option><option value='Asia/Phnom_Penh'>Asia/Phnom_Penh</option><option value='Asia/Pontianak'>Asia/Pontianak</option><option value='Asia/Pyongyang'>Asia/Pyongyang</option><option value='Asia/Qatar'>Asia/Qatar</option><option value='Asia/Qostanay'>Asia/Qostanay</option><option value='Asia/Qyzylorda'>Asia/Qyzylorda</option><option value='Asia/Rangoon'>Asia/Rangoon</option><option value='Asia/Riyadh'>Asia/Riyadh</option><option value='Asia/Saigon'>Asia/Saigon</option><option value='Asia/Sakhalin'>Asia/Sakhalin</option><option value='Asia/Samarkand'>Asia/Samarkand</option><option value='Asia/Seoul'>Asia/Seoul</option><option value='Asia/Shanghai'>Asia/Shanghai</option><option value='Asia/Singapore'>Asia/Singapore</option><option value='Asia/Srednekolymsk'>Asia/Srednekolymsk</option><option value='Asia/Taipei'>Asia/Taipei</option><option value='Asia/Tashkent'>Asia/Tashkent</option><option value='Asia/Tbilisi'>Asia/Tbilisi</option><option value='Asia/Tehran'>Asia/Tehran</option><option value='Asia/Tel_Aviv'>Asia/Tel_Aviv</option><option value='Asia/Thimbu'>Asia/Thimbu</option><option value='Asia/Thimphu'>Asia/Thimphu</option><option value='Asia/Tokyo'>Asia/Tokyo</option><option value='Asia/Tomsk'>Asia/Tomsk</option><option value='Asia/Ujung_Pandang'>Asia/Ujung_Pandang</option><option value='Asia/Ulaanbaatar'>Asia/Ulaanbaatar</option><option value='Asia/Ulan_Bator'>Asia/Ulan_Bator</option><option value='Asia/Urumqi'>Asia/Urumqi</option><option value='Asia/Ust-Nera'>Asia/Ust-Nera</option><option value='Asia/Vientiane'>Asia/Vientiane</option><option value='Asia/Vladivostok'>Asia/Vladivostok</option><option value='Asia/Yakutsk'>Asia/Yakutsk</option><option value='Asia/Yangon'>Asia/Yangon</option><option value='Asia/Yekaterinburg'>Asia/Yekaterinburg</option><option value='Asia/Yerevan'>Asia/Yerevan</option><option value='Atlantic/Azores'>Atlantic/Azores</option><option value='Atlantic/Bermuda'>Atlantic/Bermuda</option><option value='Atlantic/Canary'>Atlantic/Canary</option><option value='Atlantic/Cape_Verde'>Atlantic/Cape_Verde</option><option value='Atlantic/Faeroe'>Atlantic/Faeroe</option><option value='Atlantic/Faroe'>Atlantic/Faroe</option><option value='Atlantic/Jan_Mayen'>Atlantic/Jan_Mayen</option><option value='Atlantic/Madeira'>Atlantic/Madeira</option><option value='Atlantic/Reykjavik'>Atlantic/Reykjavik</option><option value='Atlantic/South_Georgia'>Atlantic/South_Georgia</option><option value='Atlantic/St_Helena'>Atlantic/St_Helena</option><option value='Atlantic/Stanley'>Atlantic/Stanley</option><option value='Australia/ACT'>Australia/ACT</option><option value='Australia/Adelaide'>Australia/Adelaide</option><option value='Australia/Brisbane'>Australia/Brisbane</option><option value='Australia/Broken_Hill'>Australia/Broken_Hill</option><option value='Australia/Canberra'>Australia/Canberra</option><option value='Australia/Currie'>Australia/Currie</option><option value='Australia/Darwin'>Australia/Darwin</option><option value='Australia/Eucla'>Australia/Eucla</option><option value='Australia/Hobart'>Australia/Hobart</option><option value='Australia/LHI'>Australia/LHI</option><option value='Australia/Lindeman'>Australia/Lindeman</option><option value='Australia/Lord_Howe'>Australia/Lord_Howe</option><option value='Australia/Melbourne'>Australia/Melbourne</option><option value='Australia/North'>Australia/North</option><option value='Australia/NSW'>Australia/NSW</option><option value='Australia/Perth'>Australia/Perth</option><option value='Australia/Queensland'>Australia/Queensland</option><option value='Australia/South'>Australia/South</option><option value='Australia/Sydney'>Australia/Sydney</option><option value='Australia/Tasmania'>Australia/Tasmania</option><option value='Australia/Victoria'>Australia/Victoria</option><option value='Australia/West'>Australia/West</option><option value='Australia/Yancowinna'>Australia/Yancowinna</option><option value='Brazil/Acre'>Brazil/Acre</option><option value='Brazil/DeNoronha'>Brazil/DeNoronha</option><option value='Brazil/East'>Brazil/East</option><option value='Brazil/West'>Brazil/West</option><option value='Canada/Atlantic'>Canada/Atlantic</option><option value='Canada/Central'>Canada/Central</option><option value='Canada/Eastern'>Canada/Eastern</option><option value='Canada/Mountain'>Canada/Mountain</option><option value='Canada/Newfoundland'>Canada/Newfoundland</option><option value='Canada/Pacific'>Canada/Pacific</option><option value='Canada/Saskatchewan'>Canada/Saskatchewan</option><option value='Canada/Yukon'>Canada/Yukon</option><option value='CET'>CET</option><option value='Chile/Continental'>Chile/Continental</option><option value='Chile/EasterIsland'>Chile/EasterIsland</option><option value='CST6CDT'>CST6CDT</option><option value='Cuba'>Cuba</option><option value='EET'>EET</option><option value='Egypt'>Egypt</option><option value='Eire'>Eire</option><option value='EST'>EST</option><option value='EST5EDT'>EST5EDT</option><option value='Etc/GMT'>Etc/GMT</option><option value='Etc/GMT+0'>Etc/GMT+0</option><option value='Etc/GMT+1'>Etc/GMT+1</option><option value='Etc/GMT+10'>Etc/GMT+10</option><option value='Etc/GMT+11'>Etc/GMT+11</option><option value='Etc/GMT+12'>Etc/GMT+12</option><option value='Etc/GMT+2'>Etc/GMT+2</option><option value='Etc/GMT+3'>Etc/GMT+3</option><option value='Etc/GMT+4'>Etc/GMT+4</option><option value='Etc/GMT+5'>Etc/GMT+5</option><option value='Etc/GMT+6'>Etc/GMT+6</option><option value='Etc/GMT+7'>Etc/GMT+7</option><option value='Etc/GMT+8'>Etc/GMT+8</option><option value='Etc/GMT+9'>Etc/GMT+9</option><option value='Etc/GMT-0'>Etc/GMT-0</option><option value='Etc/GMT-1'>Etc/GMT-1</option><option value='Etc/GMT-10'>Etc/GMT-10</option><option value='Etc/GMT-11'>Etc/GMT-11</option><option value='Etc/GMT-12'>Etc/GMT-12</option><option value='Etc/GMT-13'>Etc/GMT-13</option><option value='Etc/GMT-14'>Etc/GMT-14</option><option value='Etc/GMT-2'>Etc/GMT-2</option><option value='Etc/GMT-3'>Etc/GMT-3</option><option value='Etc/GMT-4'>Etc/GMT-4</option><option value='Etc/GMT-5'>Etc/GMT-5</option><option value='Etc/GMT-6'>Etc/GMT-6</option><option value='Etc/GMT-7'>Etc/GMT-7</option><option value='Etc/GMT-8'>Etc/GMT-8</option><option value='Etc/GMT-9'>Etc/GMT-9</option><option value='Etc/GMT0'>Etc/GMT0</option><option value='Etc/Greenwich'>Etc/Greenwich</option><option value='Etc/UCT'>Etc/UCT</option><option value='Etc/Universal'>Etc/Universal</option><option value='Etc/UTC'>Etc/UTC</option><option value='Etc/Zulu'>Etc/Zulu</option><option value='Europe/Amsterdam'>Europe/Amsterdam</option><option value='Europe/Andorra'>Europe/Andorra</option><option value='Europe/Astrakhan'>Europe/Astrakhan</option><option value='Europe/Athens'>Europe/Athens</option><option value='Europe/Belfast'>Europe/Belfast</option><option value='Europe/Belgrade'>Europe/Belgrade</option><option value='Europe/Berlin'>Europe/Berlin</option><option value='Europe/Bratislava'>Europe/Bratislava</option><option value='Europe/Brussels'>Europe/Brussels</option><option value='Europe/Bucharest'>Europe/Bucharest</option><option value='Europe/Budapest'>Europe/Budapest</option><option value='Europe/Busingen'>Europe/Busingen</option><option value='Europe/Chisinau'>Europe/Chisinau</option><option value='Europe/Copenhagen'>Europe/Copenhagen</option><option value='Europe/Dublin'>Europe/Dublin</option><option value='Europe/Gibraltar'>Europe/Gibraltar</option><option value='Europe/Guernsey'>Europe/Guernsey</option><option value='Europe/Helsinki'>Europe/Helsinki</option><option value='Europe/Isle_of_Man'>Europe/Isle_of_Man</option><option value='Europe/Istanbul'>Europe/Istanbul</option><option value='Europe/Jersey'>Europe/Jersey</option><option value='Europe/Kaliningrad'>Europe/Kaliningrad</option><option value='Europe/Kiev'>Europe/Kiev</option><option value='Europe/Kirov'>Europe/Kirov</option><option value='Europe/Lisbon'>Europe/Lisbon</option><option value='Europe/Ljubljana'>Europe/Ljubljana</option><option value='Europe/London'>Europe/London</option><option value='Europe/Luxembourg'>Europe/Luxembourg</option><option value='Europe/Madrid'>Europe/Madrid</option><option value='Europe/Malta'>Europe/Malta</option><option value='Europe/Mariehamn'>Europe/Mariehamn</option><option value='Europe/Minsk'>Europe/Minsk</option><option value='Europe/Monaco'>Europe/Monaco</option><option value='Europe/Moscow'>Europe/Moscow</option><option value='Europe/Nicosia'>Europe/Nicosia</option><option value='Europe/Oslo'>Europe/Oslo</option><option value='Europe/Paris'>Europe/Paris</option><option value='Europe/Podgorica'>Europe/Podgorica</option><option value='Europe/Prague'>Europe/Prague</option><option value='Europe/Riga'>Europe/Riga</option><option value='Europe/Rome'>Europe/Rome</option><option value='Europe/Samara'>Europe/Samara</option><option value='Europe/San_Marino'>Europe/San_Marino</option><option value='Europe/Sarajevo'>Europe/Sarajevo</option><option value='Europe/Saratov'>Europe/Saratov</option><option value='Europe/Simferopol'>Europe/Simferopol</option><option value='Europe/Skopje'>Europe/Skopje</option><option value='Europe/Sofia'>Europe/Sofia</option><option value='Europe/Stockholm'>Europe/Stockholm</option><option value='Europe/Tallinn'>Europe/Tallinn</option><option value='Europe/Tirane'>Europe/Tirane</option><option value='Europe/Tiraspol'>Europe/Tiraspol</option><option value='Europe/Ulyanovsk'>Europe/Ulyanovsk</option><option value='Europe/Uzhgorod'>Europe/Uzhgorod</option><option value='Europe/Vaduz'>Europe/Vaduz</option><option value='Europe/Vatican'>Europe/Vatican</option><option value='Europe/Vienna'>Europe/Vienna</option><option value='Europe/Vilnius'>Europe/Vilnius</option><option value='Europe/Volgograd'>Europe/Volgograd</option><option value='Europe/Warsaw'>Europe/Warsaw</option><option value='Europe/Zagreb'>Europe/Zagreb</option><option value='Europe/Zaporozhye'>Europe/Zaporozhye</option><option value='Europe/Zurich'>Europe/Zurich</option><option value='Factory'>Factory</option><option value='GB'>GB</option><option value='GB-Eire'>GB-Eire</option><option value='GMT'>GMT</option><option value='GMT+0'>GMT+0</option><option value='GMT-0'>GMT-0</option><option value='GMT0'>GMT0</option><option value='Greenwich'>Greenwich</option><option value='Hongkong'>Hongkong</option><option value='HST'>HST</option><option value='Iceland'>Iceland</option><option value='Indian/Antananarivo'>Indian/Antananarivo</option><option value='Indian/Chagos'>Indian/Chagos</option><option value='Indian/Christmas'>Indian/Christmas</option><option value='Indian/Cocos'>Indian/Cocos</option><option value='Indian/Comoro'>Indian/Comoro</option><option value='Indian/Kerguelen'>Indian/Kerguelen</option><option value='Indian/Mahe'>Indian/Mahe</option><option value='Indian/Maldives'>Indian/Maldives</option><option value='Indian/Mauritius'>Indian/Mauritius</option><option value='Indian/Mayotte'>Indian/Mayotte</option><option value='Indian/Reunion'>Indian/Reunion</option><option value='Iran'>Iran</option><option value='Israel'>Israel</option><option value='Jamaica'>Jamaica</option><option value='Japan'>Japan</option><option value='Kwajalein'>Kwajalein</option><option value='Libya'>Libya</option><option value='MET'>MET</option><option value='Mexico/BajaNorte'>Mexico/BajaNorte</option><option value='Mexico/BajaSur'>Mexico/BajaSur</option><option value='Mexico/General'>Mexico/General</option><option value='MST'>MST</option><option value='MST7MDT'>MST7MDT</option><option value='Navajo'>Navajo</option><option value='NZ'>NZ</option><option value='NZ-CHAT'>NZ-CHAT</option><option value='Pacific/Apia'>Pacific/Apia</option><option value='Pacific/Auckland'>Pacific/Auckland</option><option value='Pacific/Bougainville'>Pacific/Bougainville</option><option value='Pacific/Chatham'>Pacific/Chatham</option><option value='Pacific/Chuuk'>Pacific/Chuuk</option><option value='Pacific/Easter'>Pacific/Easter</option><option value='Pacific/Efate'>Pacific/Efate</option><option value='Pacific/Enderbury'>Pacific/Enderbury</option><option value='Pacific/Fakaofo'>Pacific/Fakaofo</option><option value='Pacific/Fiji'>Pacific/Fiji</option><option value='Pacific/Funafuti'>Pacific/Funafuti</option><option value='Pacific/Galapagos'>Pacific/Galapagos</option><option value='Pacific/Gambier'>Pacific/Gambier</option><option value='Pacific/Guadalcanal'>Pacific/Guadalcanal</option><option value='Pacific/Guam'>Pacific/Guam</option><option value='Pacific/Honolulu'>Pacific/Honolulu</option><option value='Pacific/Johnston'>Pacific/Johnston</option><option value='Pacific/Kanton'>Pacific/Kanton</option><option value='Pacific/Kiritimati'>Pacific/Kiritimati</option><option value='Pacific/Kosrae'>Pacific/Kosrae</option><option value='Pacific/Kwajalein'>Pacific/Kwajalein</option><option value='Pacific/Majuro'>Pacific/Majuro</option><option value='Pacific/Marquesas'>Pacific/Marquesas</option><option value='Pacific/Midway'>Pacific/Midway</option><option value='Pacific/Nauru'>Pacific/Nauru</option><option value='Pacific/Niue'>Pacific/Niue</option><option value='Pacific/Norfolk'>Pacific/Norfolk</option><option value='Pacific/Noumea'>Pacific/Noumea</option><option value='Pacific/Pago_Pago'>Pacific/Pago_Pago</option><option value='Pacific/Palau'>Pacific/Palau</option><option value='Pacific/Pitcairn'>Pacific/Pitcairn</option><option value='Pacific/Pohnpei'>Pacific/Pohnpei</option><option value='Pacific/Ponape'>Pacific/Ponape</option><option value='Pacific/Port_Moresby'>Pacific/Port_Moresby</option><option value='Pacific/Rarotonga'>Pacific/Rarotonga</option><option value='Pacific/Saipan'>Pacific/Saipan</option><option value='Pacific/Samoa'>Pacific/Samoa</option><option value='Pacific/Tahiti'>Pacific/Tahiti</option><option value='Pacific/Tarawa'>Pacific/Tarawa</option><option value='Pacific/Tongatapu'>Pacific/Tongatapu</option><option value='Pacific/Truk'>Pacific/Truk</option><option value='Pacific/Wake'>Pacific/Wake</option><option value='Pacific/Wallis'>Pacific/Wallis</option><option value='Pacific/Yap'>Pacific/Yap</option><option value='Poland'>Poland</option><option value='Portugal'>Portugal</option><option value='PRC'>PRC</option><option value='PST8PDT'>PST8PDT</option><option value='ROC'>ROC</option><option value='ROK'>ROK</option><option value='Singapore'>Singapore</option><option value='Turkey'>Turkey</option><option value='UCT'>UCT</option><option value='Universal'>Universal</option><option value='US/Alaska'>US/Alaska</option><option value='US/Aleutian'>US/Aleutian</option><option value='US/Arizona'>US/Arizona</option><option value='US/Central'>US/Central</option><option value='US/East-Indiana'>US/East-Indiana</option><option value='US/Eastern'>US/Eastern</option><option value='US/Hawaii'>US/Hawaii</option><option value='US/Indiana-Starke'>US/Indiana-Starke</option><option value='US/Michigan'>US/Michigan</option><option value='US/Mountain'>US/Mountain</option><option value='US/Pacific'>US/Pacific</option><option value='US/Samoa'>US/Samoa</option><option value='UTC'>UTC</option><option value='W-SU'>W-SU</option><option value='WET'>WET</option><option value='Zulu '>Zulu</option></select></div><br/><br/><button type='submit'>Save</button></form></main></body></html>"));
  }
}




/*
  The wifi manager is based on https://github.com/tzapu/WiFiManager


  The MIT License (MIT)

  Copyright (c) 2015 tzapu

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.


  and also code from
  https://gitlab.com/MrDIYca/code-samples/-/raw/master/esp8266_setup_portal.ino

*/
