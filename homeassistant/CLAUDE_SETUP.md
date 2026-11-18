# Zadanie pre Claude na serveri Home Assistant

Pracuješ cez SSH na mojom Ubuntu serveri s adresou 192.168.0.10. Home Assistant inštalujeme ako Home Assistant Container cez Docker Compose. Dokonči prepojenie s mojím ESP32 monitorom ovzdušia a over ukladanie histórie.

Najprv preskúmaj existujúci Docker Compose projekt, spustené kontajnery, persistované adresáre a konfiguráciu Home Assistanta. Nadviaž na existujúcu inštaláciu. Pred úpravou existujúcich konfiguračných súborov vytvor zálohu. Ak už Mosquitto alebo MQTT integrácia existujú, použi ich a doplň potrebné nastavenia.

## Kontext firmvéru

Repozitár je https://github.com/michalpaal/esp32_air_monitor a používame vetvu main. Arduino sketch je firmware/air_monitor_final/air_monitor_final.ino.

Firmware používa PubSubClient a MQTT broker 192.168.0.10:1883. Prihlasovacie údaje sa zadávajú do lokálneho mqtt_secrets.h na počítači, z ktorého sa nahráva ESP32. Súbor je v .gitignore.

- Šesť senzorov sú CO2 v ppm, teplota v °C, relatívna vlhkosť v %, PM1.0, PM2.5 a PM10 v µg/m³.
- Každé ESP32 má identifikátor air_monitor_<12 malých hex znakov Wi-Fi MAC adresy>.
- Stav odosiela každých 30 sekúnd na airmonitor/<identifikator>/state. JSON kľúče sú co2, temperature, humidity, pm1, pm25, pm10.
- Chýbajúca alebo staršia než 15 sekúnd vzorka sa posiela ako null. Nula je platné meranie.
- Discovery témy sú homeassistant/sensor/<identifikator>/<kluc>/config a majú retain.
- Availability téma je airmonitor/<identifikator>/availability, payload online/offline, s retain a Last Will offline.
- Firmware počúva homeassistant/status. Po online znovu odošle discovery a aktuálny stav.
- Senzory majú správne device_class, jednotky, state_class measurement a expire_after 90 sekúnd.
- Payload merania sa neposiela s retain. ESP32 nemá spätné doposielanie histórie po výpadku siete.
- Lokálny web a e-paper fungujú súbežne s MQTT. Štartovacie ustaľovanie teploty ostáva súčasťou meraní.

## Vykonaj na serveri

1. Ak ešte broker nebeží, pridaj Eclipse Mosquitto 2 do existujúceho Compose projektu. Publikuj TCP 1883 na hoste 192.168.0.10. Nastav restart unless-stopped a trvalé úložisko pre konfiguráciu aj /mosquitto/data. Over vlastníctvo a oprávnenia adresárov.
2. Nastav listener 1883, allow_anonymous false, password_file, persistence true a persistence_location. Pri existujúcom brokeri zachovaj funkčné účty a nastavenia. Vytvor účet esp32_air_monitor so silným heslom. Home Assistant nech používa vlastný účet. Heslá drž mimo Git repozitára a výpisov logov. Ak sú ACL, povoľ príslušným účtom potrebný prístup k airmonitor/# a homeassistant/#.
3. Spusti alebo aktualizuj broker a over publikovanie aj odber správ autentifikovaným klientom. Pri testovaní discovery používaj dočasné témy a po teste odstráň ich retained správy, ak vznikli.
4. Pripoj MQTT integráciu v Home Assistante k 192.168.0.10:1883. Ponechaj discovery prefix homeassistant a birth topic homeassistant/status s payloadom online. Použi existujúci autorizovaný prístup k HA. Ak treba krok v prihlásenom webovom rozhraní, priprav server a potom mi presne vypíš potrebné kliknutia. Upravovanie interného .storage nahraď podporovaným nastavením integrácie.
5. Over Recorder, History a trvalé uloženie HA /config. Pri predvolenom SQLite musí byť persistovaný aj súbor home-assistant_v2.db. Ak už používa externú databázu, použi existujúce úložisko. Nastav purge_keep_days na 30 zlúčením s existujúcou sekciou recorder. Ak už má dlhšiu retenciu, zachovaj ju. Skontroluj include/exclude filtre, aby šesť senzorov monitoru bolo zaznamenávaných a mali dlhodobé štatistiky. Pred reštartom over konfiguráciu postupom podporovaným danou inštaláciou.
6. Nastav potrebné lokálne sieťové pravidlá, aby ESP32 aj Home Assistant dosiahli broker. Over lokálne adresy a prípadnú izoláciu Wi-Fi klientov podľa dostupných informácií.
7. Priprav mi hodnoty pre MQTT_HOST, MQTT_PORT, MQTT_USERNAME a bezpečný spôsob získania MQTT_PASSWORD do lokálneho mqtt_secrets.h. Použi host 192.168.0.10 a port 1883. Na Arduino strane je potrebné nainštalovať PubSubClient od Nicka O'Learyho a nahrať aktuálny main. Zatiaľ nemám potvrdené, že nový firmware je nahratý.

## Over výsledok

Over docker compose config, stav brokeru, reštartovanie služby a persistenciu jeho dát. Skontroluj logy bez vypísania hesiel. Po nahratí ESP32 musí v MQTT prísť discovery pre šesť senzorov a stav s reálnymi hodnotami, v HA zariadenie Air Monitor a aktualizácie približne každých 30 sekúnd.

Keď sú k dispozícii skutočné merania, over ich v HA History, reštartuj Home Assistant a potvrď zachovanie predchádzajúcich záznamov aj návrat nových hodnôt. Hodinové dlhodobé štatistiky potrebujú čas na vytvorenie. Ak ESP32 ešte nový firmware nemá, dokonči všetku dostupnú prácu a označ tento posledný test ako čakajúci na upload. Nespúšťaj falošné merania do skutočných senzorov.

Na záver uveď, ktoré súbory si zmenil, kde sú trvalé dáta, ktoré služby bežia, výsledky testov a čo presne mám urobiť pri nahratí ESP32. Rozlišuj overené výsledky a kroky čakajúce na fyzické zariadenie.
