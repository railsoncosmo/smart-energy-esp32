#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include "EmonLib.h"

#define WIFI_SSID ""
#define WIFI_PASSWORD ""

#define PINO_SCT 32           
#define TENSAO_REDE 220.0     
#define CALIBRACAO_SCT 20.0  

#define PINO_LED_VERMELHO 26  
#define PINO_LED_VERDE 27     
#define PINO_BUZZER 14        

#define API_KEY ""
#define FIREBASE_PROJECT_ID ""

#define USER_EMAIL ""
#define USER_PASSWORD ""

EnergyMonitor emon1;          
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long tempoAnterior = 0;
const long intervalo = 5000;
String chipName = ESP.getChipModel();
uint32_t freqCpu = ESP.getCpuFreqMHz();

void setup() {
  Serial.begin(115200);
  delay(500);

  // Configuração dos pinos dos LEDs e Buzzer como saídas digitais
  pinMode(PINO_LED_VERMELHO, OUTPUT);
  pinMode(PINO_LED_VERDE, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);

  digitalWrite(PINO_LED_VERMELHO, LOW);
  digitalWrite(PINO_BUZZER, LOW);
  digitalWrite(PINO_LED_VERDE, HIGH);

  analogSetAttenuation(ADC_11db); 

  // Inicialização e Calibração do Sensor SCT-013
  emon1.current(PINO_SCT, CALIBRACAO_SCT);

  Serial.println("\n[Sensor] Efetuando leituras de estabilização do hardware...");
  for (int i = 0; i < 5; i++) {
    emon1.calcIrms(1480);
    delay(200);
  }
  Serial.println("[Sensor] Hardware estabilizado com sucesso!");

  // Inicializa conexão Wi-Fi de forma correta (esperando conectar)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("\nWi-Fi Conectado com sucesso!");
  Serial.print("O modelo do chip é: ");
  Serial.println(chipName);

  config.api_key = API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  config.token_status_callback = tokenStatusCallback;

  config.signer.signupError.message.clear();

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  delay(1000);
}

void loop() {  
  static int powerLimitValue = 0;
  static String response = "";
  static FirebaseJson jsonParser;
  static bool encontrado = false;
  static int totalSensores = 0;
  static bool primeiroCicloLeitura = true;

  if (Firebase.ready() && (millis() - tempoAnterior >= intervalo || tempoAnterior == 0)) {
    tempoAnterior = millis();

    if(chipName.length() > 0 && totalSensores < 1){
      FirebaseJson sensor;
      sensor.set("fields/name/stringValue", chipName);
      sensor.set("fields/frequencia/integerValue", freqCpu);
      String nameCollection = "sensores";
      
      if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", nameCollection.c_str(), sensor.raw())) {
        Serial.println("Sensor Criado com Sucesso!");
        totalSensores = 1;
      } else {
        Serial.printf("Erro ao criar sensor: %s\n", fbdo.errorReason().c_str());
      }
      Serial.println(fbdo.payload());
    }

    String collection = "aparelhos";
    FirebaseJson query;

    query.set("from/[0]/collectionId", collection);
    query.set("where/fieldFilter/field/fieldPath", "selected");
    query.set("where/fieldFilter/op", "EQUAL");
    query.set("where/fieldFilter/value/booleanValue", true);

    Serial.println("\n[Firestore] Executando Query Parameter...");
    
    encontrado = false;

    if(Firebase.Firestore.runQuery(&fbdo, FIREBASE_PROJECT_ID, String("").c_str(), String("").c_str(), &query)){
      response = fbdo.payload().c_str();

      jsonParser.setJsonData(response);
      FirebaseJsonData document;
 
      if(jsonParser.get(document, "[0]/document/fields/powerLimit/doubleValue")){
        powerLimitValue = (int)document.floatValue;
        encontrado = true;
        Serial.printf("Aparelho Ativo! Limite de Potência: %d W (double)\n", powerLimitValue);
      }
      else if(jsonParser.get(document, "[0]/document/fields/powerLimit/integerValue")){
        powerLimitValue = document.intValue;
        encontrado = true;
        Serial.printf("Aparelho Ativo! Limite de Potência: %d W (int)\n", powerLimitValue);
      } 
    } else {
      Serial.printf("Erro HTTP na Query: %d | Motivo: %s\n", fbdo.httpCode(), fbdo.errorReason().c_str());
    }

    if (!encontrado) {
      Serial.println("Sistema em Standby: Aguardando seleção de aparelho no Firestore.");
      
      digitalWrite(PINO_LED_VERDE, LOW);
      digitalWrite(PINO_LED_VERMELHO, LOW);
      noTone(PINO_BUZZER);
      return;
    }

    // Leitura real por amostragem RMS.
    double maiorCorrente = emon1.calcIrms(1480);

    // Filtro de Ruído
    if (maiorCorrente < 0.80) {
      maiorCorrente = 0.0;
    }

    float correnteAmpere = (float)maiorCorrente;
    float potenciaWatts = correnteAmpere * TENSAO_REDE;

    if (primeiroCicloLeitura) {
      primeiroCicloLeitura = false;
      return; 
    }

    if (potenciaWatts >= powerLimitValue) {
      digitalWrite(PINO_LED_VERDE, LOW);       
      digitalWrite(PINO_LED_VERMELHO, HIGH);   
      tone(PINO_BUZZER, 2500, 200);
      
      Serial.printf("[ALERTA]: Potência de %.2f W acima do limite (%d W)!\n", potenciaWatts, powerLimitValue);

      FirebaseJsonData docIdData;
      if (jsonParser.get(docIdData, "[0]/document/name")) {
        String fullName = docIdData.stringValue.c_str();
        int ultimaBarra = fullName.lastIndexOf('/');
        String idAparelho = fullName.substring(ultimaBarra + 1);

        String idPico = "pico_" + String(millis());
        String caminhoSubcolecao = "aparelhos/" + idAparelho + "/historico_picos/" + idPico;

        FirebaseJson picoPayload;
        picoPayload.set("fields/correnteRegistrada/doubleValue", correnteAmpere);
        picoPayload.set("fields/potenciaRegistrada/doubleValue", potenciaWatts);

        Serial.println("[Pico de Energia] Registrando pico de energia!");
        
        if (Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", caminhoSubcolecao.c_str(), picoPayload.raw(), "fields")) {
          Serial.println("Sucesso! Evento de pico salvo.");
        } else {
          Serial.printf("Erro ao salvar: %d | Motivo: %s\n", fbdo.httpCode(), fbdo.errorReason().c_str());
        }
      }
    } else {
      digitalWrite(PINO_LED_VERDE, HIGH);      
      digitalWrite(PINO_LED_VERMELHO, LOW);    
      noTone(PINO_BUZZER);
      
      Serial.printf("Monitoramento Normal: %.2f W / Limite: %d W\n", potenciaWatts, powerLimitValue);
    }
  }
}