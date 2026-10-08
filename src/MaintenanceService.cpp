#include "MaintenanceService.h"
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

namespace {
static_assert(sizeof(esp_app_desc_t) == 256, "Verifier le format d'image OTA de cette version ESP-IDF");
String randomToken() {
  uint8_t bytes[16]; esp_fill_random(bytes, sizeof(bytes));
  String token; token.reserve(32); char hex[3];
  for (auto byte : bytes) { snprintf(hex, sizeof(hex), "%02x", byte); token += hex; }
  return token;
}
String escapeHtml(String text) {
  text.replace("&", "&amp;"); text.replace("<", "&lt;"); text.replace(">", "&gt;");
  text.replace("\"", "&quot;"); text.replace("'", "&#39;"); return text;
}
}

void MaintenanceService::begin() {
  homeSsid_ = prefs_.getString("home_ssid", "");
  homePassword_ = prefs_.isKey("home_pass") ? prefs_.getString("home_pass") : "";
  otaPassword_ = prefs_.isKey("ota_pass") ? prefs_.getString("ota_pass") : "";
  if (otaPassword_.length() < 12) {
    otaPassword_ = randomToken();
    if (!prefs_.putString("ota_pass", otaPassword_))
      error_ = "Echec sauvegarde code OTA : configurer un code avant usage distant";
  }
  csrf_ = randomToken(); // jamais en MQTT, JSON /status ou journal
}

BootMode MaintenanceService::consumeBootMode() {
  const uint8_t value = prefs_.getUChar("bootmode", 0);
  if (value) prefs_.putUChar("bootmode", 0); // RESET/power-cycle revient normal
  return validBootMode(value) ? static_cast<BootMode>(value) : BootMode::Normal;
}

void MaintenanceService::setMode(BootMode mode) { mode_ = mode; }

bool MaintenanceService::queueReboot(BootMode mode) {
  if (!validBootMode(static_cast<uint8_t>(mode)) || updating()) return false;
  // Wi-Fi maison non configure : AP plutot qu'un redemarrage inaccessible.
  if (mode == BootMode::HomeWifi && !homeConfigured()) mode = BootMode::AccessPoint;
  if (prefs_.putUChar("bootmode", static_cast<uint8_t>(mode)) == 0) {
    error_ = "Impossible de memoriser le mode de redemarrage"; return false;
  }
  prefs_.putBool("portalboot", false);
  prefs_.remove("soloboot");
  rebootQueued_ = true; rebootAtMs_ = millis() + 800;
  Serial.printf("Maintenance: redemarrage programme mode=%u\n", static_cast<unsigned>(mode));
  return true;
}

void MaintenanceService::tick() {
  if (uploadActive_ && static_cast<uint32_t>(millis() - uploadLastMs_) >= 120000)
    abortUpload("Upload interrompu depuis 120 s; ancien firmware conserve");
  if (rebootQueued_ && !uploadActive_ && static_cast<int32_t>(millis() - rebootAtMs_) >= 0)
    ESP.restart();
}

bool MaintenanceService::authenticate() {
  if (web_.authenticate("admin", otaPassword_.c_str())) return true;
  web_.requestAuthentication(BASIC_AUTH, "PHEV maintenance"); return false;
}

bool MaintenanceService::tokenValid() const {
  return web_.arg("token") == csrf_ || web_.header("X-CSRF-Token") == csrf_;
}

String MaintenanceService::controlsHtml() const {
  String html = "<fieldset><legend>Maintenance / mise a jour OTA</legend>";
  html += "<p>Mode actuel : " + String(homeMode() ? "Wi-Fi maison" : "point d'acces") +
          ". Zigbee et voiture suspendus en maintenance.</p>";
  html += "<small>Image cible : " + String(PHEV_IMAGE_ID) + "</small>";
  if (error_.length()) html += "<p>" + escapeHtml(error_) + "</p>";
  html += "<form method='post' action='/maintenance/save'><label>SSID maintenance</label>";
  html += "<input name='home_ssid' maxlength='32' required value='" + escapeHtml(homeSsid_) + "'>";
  html += "<label>Cle Wi-Fi maison</label><input name='home_pass' type='password' maxlength='63' placeholder='vide = conserver'>";
  html += "<label>Nouveau code maintenance / OTA (12 caracteres minimum)</label>";
  html += "<input name='ota_pass' type='password' minlength='12' maxlength='64' placeholder='vide = conserver'>";
  html += "<button>Enregistrer sans redemarrer</button></form>";
  html += "<p>Authentification web/OTA : utilisateur <code>admin</code>. ";
  if (!homeMode()) {
    // Accessible uniquement sur l'AP WPA2, jamais sur le reseau voiture ou en JSON.
    html += "<details><summary>Afficher le code maintenance actuel</summary><code>" + escapeHtml(otaPassword_) + "</code></details>";
  } else html += "Utiliser le code configure dans le point d'acces.";
  html += "</p><p><a href='/update'>Televerser firmware.bin</a> (pas firmware.factory.bin)</p>";
  const char *names[] = {"Redemarrer en mode normal", "Redemarrer en maintenance AP", "Redemarrer en maintenance Wi-Fi maison"};
  for (uint8_t i = 0; i < 3; ++i) {
    html += "<form method='post' action='/restart'><input type='hidden' name='mode' value='" + String(i) +
            "'><button>" + String(names[i]) + "</button></form>";
  }
  return html + "<small>Retour normal apres 30 min. Si le Wi-Fi maison est indisponible : repli AP apres 30 s. Aucune commande clim au redemarrage.</small></fieldset>";
}

void MaintenanceService::decoratePage(String &page) const {
  const String hidden = "<input type='hidden' name='token' value='" + csrf_ + "'></form>";
  page.replace("</form>", hidden);
  const String script = "<script>const phevCsrf='" + csrf_ + "';const phevFetch=window.fetch;"
    "window.fetch=(url,opt={})=>{if((opt.method||'GET').toUpperCase()==='POST')"
    "opt.headers={...(opt.headers||{}),'X-CSRF-Token':phevCsrf};return phevFetch(url,opt);};</script>";
  // Ajouter avant les scripts existants sans remplacer leurs declarations.
  const int position = page.indexOf("<script>");
  if (position >= 0) page = page.substring(0, position) + script + page.substring(position);
  else page += script;
}

void MaintenanceService::abortUpload(const char *error) {
  if (Update.isRunning()) Update.abort();
  uploadActive_ = false; uploadOk_ = false; error_ = error;
}

void MaintenanceService::uploadChunk() {
  HTTPUpload &upload = web_.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadOk_ = false; uploadAuthorized_ = false; error_ = "";
    if (!active() || !authenticate() || !tokenValid()) { error_ = "Authentification/token OTA requis"; return; }
    if (uploadActive_) { abortUpload("Upload deja actif"); return; }
    if (!upload.filename.endsWith(".bin")) { error_ = "Choisir firmware.bin"; return; }
    expectedSize_ = static_cast<size_t>(web_.arg("size").toInt());
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target || expectedSize_ < sizeof(prefix_) || expectedSize_ > target->size) {
      error_ = "Taille firmware invalide ou partition OTA absente"; return;
    }
    uploadAuthorized_ = true; uploadActive_ = true; receivedSize_ = prefixSize_ = 0;
    prefixChecked_ = false; uploadLastMs_ = millis();
    identityFound_ = false; identityMatched_ = 0;
    if (beforeUpdate_) beforeUpdate_();
  } else if (upload.status == UPLOAD_FILE_WRITE && uploadAuthorized_ && uploadActive_) {
    uploadLastMs_ = millis();
    if (receivedSize_ + upload.currentSize > expectedSize_) { abortUpload("Fichier plus grand que la taille annoncee"); return; }
    receivedSize_ += upload.currentSize;
    if (!identityFound_) identityFound_ = scanPhevIdentity(upload.buf, upload.currentSize, identityMatched_);
    size_t offset = 0;
    if (!prefixChecked_) {
      const size_t available = sizeof(prefix_) - prefixSize_;
      const size_t copied = upload.currentSize < available ? upload.currentSize : available;
      memcpy(prefix_ + prefixSize_, upload.buf, copied); prefixSize_ += copied; offset = copied;
      if (prefixSize_ < sizeof(prefix_)) return;
      if (!validC6ApplicationPrefix(prefix_, prefixSize_)) { abortUpload("Image invalide : application ESP32-C6 requise, pas image factory/bootloader"); return; }
      const esp_app_desc_t *running = esp_app_get_description();
      esp_app_desc_t candidate; memcpy(&candidate, prefix_ + 32, sizeof(candidate));
      if (memcmp(candidate.project_name, running->project_name, sizeof(candidate.project_name)) != 0) {
        abortUpload("Image d'un autre projet : mise a jour refusee"); return;
      }
      if (!Update.begin(expectedSize_, U_FLASH) || Update.write(prefix_, prefixSize_) != prefixSize_) {
        abortUpload("Impossible d'ecrire la partition OTA"); return;
      }
      prefixChecked_ = true;
    }
    const size_t remaining = upload.currentSize - offset;
    if (remaining && Update.write(upload.buf + offset, remaining) != remaining)
      abortUpload("Erreur ecriture OTA; ancien firmware conserve");
  } else if (upload.status == UPLOAD_FILE_END && uploadAuthorized_ && uploadActive_) {
    if (receivedSize_ != expectedSize_ || !prefixChecked_) { abortUpload("Fichier incomplet; ancien firmware conserve"); return; }
    if (!identityFound_) { abortUpload("Identite PHEV Remote C6 absente : image d'un autre projet refusee"); return; }
    uploadOk_ = Update.end(false); uploadActive_ = false;
    if (!uploadOk_) error_ = "Verification de l'image echouee; ancien firmware conserve";
  } else if (upload.status == UPLOAD_FILE_ABORTED) abortUpload("Upload annule; ancien firmware conserve");
}

void MaintenanceService::installRoutes() {
  const char *headers[] = {"X-CSRF-Token"}; web_.collectHeaders(headers, 1);
  web_.addMiddleware([this](WebServer &server, Middleware::Callback next) {
    if (!active()) { server.send(403, "text/plain", "Maintenance uniquement"); return false; }
    if ((homeMode() || server.uri() == "/update") && !authenticate()) return false;
    if (server.method() == HTTP_POST && !tokenValid()) { server.send(403, "text/plain", "Token de la page expire : recharger"); return false; }
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Frame-Options", "DENY");
    return next();
  });
  web_.on("/maintenance/save", HTTP_POST, [this] {
    if (updating()) { web_.send(409, "text/plain", "OTA en cours"); return; }
    const String ssid = web_.arg("home_ssid");
    String password = web_.arg("home_pass"), ota = web_.arg("ota_pass");
    if (!password.length()) password = homePassword_;
    if (!ota.length()) ota = otaPassword_;
    if (!ssid.length() || ssid.length() > 32 || password.length() < 8 || password.length() > 63 || ota.length() < 12 || ota.length() > 64) {
      web_.send(400, "text/plain", "SSID/cle Wi-Fi invalide; code OTA de 12 a 64 caracteres"); return;
    }
    if (!prefs_.putString("home_ssid", ssid) || !prefs_.putString("home_pass", password) || !prefs_.putString("ota_pass", ota)) {
      web_.send(500, "text/plain", "Echec sauvegarde maintenance"); return;
    }
    homeSsid_ = ssid; homePassword_ = password; otaPassword_ = ota;
    web_.send(200, "text/html; charset=utf-8", "Enregistre. <a href='/'>Retour</a>. Nouveau code requis pour les prochaines connexions.");
  });
  web_.on("/restart", HTTP_POST, [this] {
    const String value = web_.arg("mode");
    if (value != "0" && value != "1" && value != "2") { web_.send(400, "text/plain", "Mode invalide"); return; }
    if (!queueReboot(static_cast<BootMode>(value.toInt()))) { web_.send(409, "text/plain", "OTA en cours ou sauvegarde impossible"); return; }
    web_.send(200, "text/plain", "Redemarrage programme. Reconnecter le navigateur au reseau choisi.");
  });
  web_.on("/update", HTTP_GET, [this] {
    String page = "<!doctype html><html lang='fr'><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
      "<title>OTA PHEV C6</title><h1>Mise a jour PHEV C6</h1><p>Application ESP32-C6 : firmware.bin uniquement. Ne pas couper l'alimentation.</p>"
      "<form id='ota' method='post' enctype='multipart/form-data'><input id='file' type='file' name='firmware' accept='.bin' required><button>Televerser</button></form>"
      "<script>document.getElementById('ota').onsubmit=function(){let f=document.getElementById('file').files[0];this.action='/update?token=" + csrf_ +
      "&size='+f.size;document.querySelector('button').disabled=true;};</script><p><a href='/'>Retour</a></p></html>";
    web_.send(200, "text/html; charset=utf-8", page);
  });
  web_.on("/update", HTTP_POST, [this] {
    if (!uploadOk_) { web_.send(400, "text/plain", error_.length() ? error_ : "Aucun firmware recu"); return; }
    if (!queueReboot(BootMode::Normal)) { web_.send(500, "text/plain", "Image chargee; redemarrage manuel requis"); return; }
    web_.send(200, "text/plain", "Firmware verifie. Redemarrage en mode normal; association Zigbee conservee.");
  }, [this] { uploadChunk(); });
}
