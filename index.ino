#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Fingerprint.h>
#include <RTClib.h>
#include <EEPROM.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>

// ============= AI ASSISTANT MODES =============
// 0 = Offline (built-in keyword rules, no network needed at all)
// 1 = Cloud AI (real LLM over the internet - ESP32 also joins a router's WiFi
//     alongside its own self-hosted AP, so the device stays reachable offline
//     even if the internet link drops)
// 2 = Local AI (real LLM running on a Raspberry Pi/PC on the same local
//     network - no internet required, just a device running an LLM server
//     such as Ollama, connected to this ESP32's own WiFi network)
struct AssistantSettings {
  uint8_t mode = 0;
  String wifiSSID = "";
  String wifiPassword = "";
  String apiKey = "";
  String apiEndpoint = "https://api.anthropic.com/v1/messages";
  String apiModel = "claude-haiku-4-5-20251001";
  String localURL = "";       // e.g. http://192.168.4.50:11434
  String localModel = "llama3.2";
};
AssistantSettings assistantSettings;
Preferences assistantPrefs;

// ============= SELF-HOSTED NETWORK (SoftAP) =============
// No router or internet needed - the ESP32 hosts its own network.
// The name/password below are set directly in WiFi.softAP(...) in setup().
// Change them there if you want a different network name/password.

// ============= TIMEZONE CONFIGURATION =============
const long utcOffsetInSeconds = 28800; // UTC+8 for Philippines (8 hours * 3600 seconds)

// ============= WEBSERVER =============
WebServer server(80);

// ============= HARDWARE =============
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;

// ============= TIME CONFIGURATION =============
struct TimeConfig {
  int startHour;
  int startMinute;
  int endHour;
  int endMinute;
  int gracePeriod; // minutes
  bool autoDetect; // automatically detect based on time of day
};

// Default time configuration
TimeConfig timeConfig = {
  8, 0,  // Start time: 8:00 AM
  17, 0, // End time: 5:00 PM
  15,    // Grace period: 15 minutes
  true   // Auto-detect Time In/Out based on time of day
};

// ============= DATA STRUCTURES =============
struct Student {
  int id;
  char name[20];
  char grade[5];
  char section[9];  // 8 characters + null terminator
  bool isRegistered;
};

// Location options - UPDATED with requested changes
const char* locationNames[] = {
  "Admission Office",  // Changed from "Admin Office"
  "Registrar",
  "CR",
  "Cashier",
  "Canteen",
  "Library",
  "Clinic",
  "Guidance Office"    // Added Guidance Office
};

// Update location count constant
const int LOCATION_COUNT = 8;  // Increased from 7 to 8

// Attendance remark types
const char* remarkTypes[] = {
  "On Time",
  "Late",
  "Early",
  "Overtime",
  "Normal"
};

struct Attendance {
  int studentId;
  char timeIn[12];  // Increased to 12 for AM/PM format
  char timeOut[12]; // Increased to 12 for AM/PM format
  char date[11];
  int location;
  bool isActive;
  int remark; // 0=On Time, 1=Late, 2=Early, 3=Overtime, 4=Normal (for time out)
  int duration; // duration in minutes
};

// ============= CONSTANTS =============
const int MAX_STUDENTS = 50;
const int MAX_ATTENDANCE = 200;
const int EEPROM_SIZE = 4096;
const int MAX_FINGERPRINT_CAPACITY = 162; // R307 sensor max capacity

// Memory layout
const int STUDENT_COUNT_ADDR = 0;
const int STUDENT_DATA_START = 4;
const int STUDENT_RECORD_SIZE = 40;
const int MAX_STUDENT_MEMORY = STUDENT_RECORD_SIZE * MAX_STUDENTS;

const int ATTENDANCE_COUNT_ADDR = STUDENT_DATA_START + MAX_STUDENT_MEMORY;
const int ATTENDANCE_DATA_START = ATTENDANCE_COUNT_ADDR + 4;
const int ATTENDANCE_RECORD_SIZE = 48; // Increased to accommodate larger time strings
const int MAX_ATTENDANCE_MEMORY = ATTENDANCE_RECORD_SIZE * MAX_ATTENDANCE;

const int TIME_CONFIG_ADDR = ATTENDANCE_DATA_START + MAX_ATTENDANCE_MEMORY;

// ============= OPTIMIZATION CONSTANTS =============
const int SCAN_INTERVAL_MS = 50;
const int LCD_UPDATE_INTERVAL = 2000;
const int WEBSERVER_PROCESS_INTERVAL = 5;

// ============= GLOBAL VARIABLES =============
Student students[MAX_STUDENTS];
Attendance attendanceRecords[MAX_ATTENDANCE];
int studentCount = 0;
int attendanceCount = 0;
String currentScanResult = "";
bool fingerprintMode = false;
bool registrationMode = false;
unsigned long lastScanCheck = 0;
unsigned long lastLCDUpdate = 0;
String pendingName = "";
String pendingGrade = "";
String pendingSection = "";
int pendingId = 0;
int registrationStep = 1;  // Start at step 1
unsigned long registrationStartTime = 0;
int selectedLocation = 0;
int lastSuccessId = 0;

// Cache for quick lookup
int fingerprintToStudentId[200] = {0};
int studentIdToIndex[201] = {0};

// ============= FUNCTION DECLARATIONS =============
void showMainMenu();
void setupWebServer();
void loadStudents();
void loadAttendance();
void loadTimeConfig();
void saveStudents();
void saveAttendance();
void saveTimeConfig();
void buildFingerprintCache();
void rebuildStudentCache();
int getNextAvailableId();
Student* findStudentById(int id);
bool isCheckedInToday(int studentId);
void recordAttendance(int studentId, bool isTimeIn, int location);
String getLocationName(int location);
String getRemarkText(int remark);
int calculateRemark(DateTime scanTime, bool isTimeIn);
int calculateDuration(const char* timeIn, const char* timeOut);
void quickScan();
void handleRegistration();
void resetRegistration();
void checkSensorMemory();
void checkSensorCapacity();
String getErrorMessage(uint8_t errorCode);
void migrateEEPROM();
void forceResetEEPROM();
bool validateAndFixRTC();
bool syncTimeWithNTP();
void setManualTime(int year, int month, int day, int hour, int minute, int second);
void checkRTC();
void safeTimeFormat(char* buffer, int hour, int minute, int second);
void safeDateFormat(char* buffer, int year, int month, int day);
String processAssistantQuery(String query);
String buildAttendanceContext();
String getCloudAIAnswer(String query);
String getLocalAIAnswer(String query);
String jsonEscape(String input);
String extractJsonStringValue(String json, String key);
void loadAssistantSettings();
void saveAssistantSettings();
bool ensureInternetConnected();

// ============= HTML PAGE =============
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>CFSI Monitoring System</title>
    <link href="https://fonts.googleapis.com/css2?family=Poppins:wght@300;400;600;700;800&display=swap" rel="stylesheet">
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Poppins:wght@300;400;600;700;800&display=swap');
        
        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }
        
        body { 
            font-family: 'Poppins', 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 0; 
            padding: 20px; 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
        }
        
        .container { 
            max-width: 1300px; 
            margin: 0 auto; 
            background: white; 
            padding: 30px; 
            border-radius: 20px; 
            box-shadow: 0 20px 60px rgba(0,0,0,0.3); 
        }
        
        h1 { 
            text-align: center; 
            margin-bottom: 30px;
            padding-bottom: 15px;
            font-size: 2.8em;
            font-weight: 800;
            letter-spacing: 2px;
            text-transform: uppercase;
            background: linear-gradient(135deg, #667eea, #764ba2, #ff6b6b, #4ecdc4);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            background-clip: text;
            text-shadow: 2px 4px 10px rgba(0,0,0,0.2);
            position: relative;
            animation: titleGlow 3s ease-in-out infinite;
        }
        
        @keyframes titleGlow {
            0%, 100% {
                filter: drop-shadow(0 0 5px rgba(102,126,234,0.5));
            }
            50% {
                filter: drop-shadow(0 0 20px rgba(118,75,162,0.8));
            }
        }
        
        h1::before {
            content: "ðŸ” ";
            font-size: 1.2em;
            background: linear-gradient(135deg, #ff6b6b, #4ecdc4);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        
        h1::after {
            content: " ðŸ“Š";
            font-size: 1.2em;
            background: linear-gradient(135deg, #4ecdc4, #ff6b6b);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        
        .stats { 
            display: grid; 
            grid-template-columns: repeat(4, 1fr); 
            gap: 20px; 
            margin: 30px 0; 
        }
        
        .stat-card { 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white; 
            padding: 20px; 
            border-radius: 15px; 
            text-align: center;
            box-shadow: 0 10px 20px rgba(102,126,234,0.3);
            transition: transform 0.3s, box-shadow 0.3s;
            cursor: pointer;
        }
        
        .stat-card:hover {
            transform: translateY(-5px);
            box-shadow: 0 15px 30px rgba(102,126,234,0.5);
        }
        
        .stat-number { 
            font-size: 2.5em; 
            font-weight: bold; 
            margin-bottom: 5px;
            text-shadow: 2px 2px 4px rgba(0,0,0,0.2);
        }
        
        .stat-label {
            font-size: 1em;
            opacity: 0.9;
            text-transform: uppercase;
            letter-spacing: 1px;
        }
        
        .menu {
            display: grid;
            grid-template-columns: repeat(4, 1fr);
            gap: 12px;
            margin: 30px 0;
        }
        
        .btn {
            background: #667eea;
            color: white;
            border: none;
            padding: 15px 10px;
            border-radius: 10px;
            font-size: 1em;
            font-weight: 500;
            cursor: pointer;
            transition: all 0.3s;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 8px;
        }
        
        .btn:hover {
            background: #764ba2;
            transform: translateY(-2px);
            box-shadow: 0 6px 12px rgba(102,126,234,0.4);
        }
        
        .btn-danger {
            background: #e74c3c;
        }
        
        .btn-danger:hover {
            background: #c0392b;
        }
        
        .btn-success {
            background: #27ae60;
        }
        
        .btn-success:hover {
            background: #219a52;
        }
        
        .btn-warning {
            background: #f39c12;
        }
        
        .btn-warning:hover {
            background: #e67e22;
        }
        
        .btn-reset {
            background: #e74c3c;
            font-weight: bold;
            border: 2px solid #c0392b;
        }
        
        .location-selector {
            display: grid;
            grid-template-columns: repeat(4, 1fr);
            gap: 12px;
            margin: 20px 0;
        }
        
        .location-btn {
            background: #f8f9fa;
            border: 2px solid #dee2e6;
            padding: 15px 10px;
            border-radius: 10px;
            cursor: pointer;
            transition: all 0.3s;
            font-weight: 500;
            text-align: center;
        }
        
        .location-btn:hover {
            background: #e9ecef;
            border-color: #adb5bd;
        }
        
        .location-btn.selected {
            background: #667eea;
            color: white;
            border-color: #667eea;
            box-shadow: 0 4px 10px rgba(102,126,234,0.3);
        }
        
        .scan-area {
            background: #f8f9fa;
            padding: 40px 30px;
            border-radius: 15px;
            text-align: center;
            margin: 30px 0;
            border: 3px dashed #667eea;
        }
        
        .fingerprint-icon {
            font-size: 5em;
            color: #667eea;
            animation: pulse 2s infinite;
            margin-bottom: 20px;
        }
        
        @keyframes pulse {
            0% { transform: scale(1); opacity: 1; }
            50% { transform: scale(1.1); opacity: 0.8; }
            100% { transform: scale(1); opacity: 1; }
        }
        
        /* Assistant Chat Styles */
        .chat-container {
            background: #f8f9fa;
            border-radius: 15px;
            border: 2px solid #e9ecef;
            overflow: hidden;
            margin: 20px 0;
        }

        .chat-messages {
            height: 380px;
            overflow-y: auto;
            padding: 20px;
            display: flex;
            flex-direction: column;
            gap: 12px;
        }

        .chat-msg {
            max-width: 80%;
            padding: 10px 15px;
            border-radius: 15px;
            line-height: 1.4;
            font-size: 0.95em;
            white-space: pre-line;
        }

        .chat-msg-user {
            align-self: flex-end;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border-bottom-right-radius: 3px;
        }

        .chat-msg-bot {
            align-self: flex-start;
            background: white;
            color: #333;
            border: 1px solid #e9ecef;
            border-bottom-left-radius: 3px;
        }

        .chat-suggestions {
            display: flex;
            flex-wrap: wrap;
            gap: 8px;
            padding: 0 20px 15px 20px;
        }

        .chat-chip {
            background: white;
            border: 1px solid #667eea;
            color: #667eea;
            padding: 6px 12px;
            border-radius: 20px;
            font-size: 0.8em;
            cursor: pointer;
            transition: all 0.2s;
        }

        .chat-chip:hover {
            background: #667eea;
            color: white;
        }

        .chat-input-row {
            display: flex;
            gap: 10px;
            padding: 15px;
            background: white;
            border-top: 2px solid #e9ecef;
        }

        .chat-input-row input {
            flex: 1;
            padding: 12px 15px;
            border: 2px solid #e9ecef;
            border-radius: 25px;
            font-size: 0.95em;
            font-family: 'Poppins', sans-serif;
        }

        .chat-input-row input:focus {
            outline: none;
            border-color: #667eea;
        }

        .chat-send-btn {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            border-radius: 25px;
            padding: 0 22px;
            font-weight: 600;
            cursor: pointer;
            font-family: 'Poppins', sans-serif;
        }

        /* Table Styles */
        .table-container {
            overflow-x: auto;
            margin: 25px 0;
            border-radius: 12px;
            box-shadow: 0 5px 15px rgba(0,0,0,0.08);
        }
        
        table {
            width: 100%;
            border-collapse: collapse;
            background: white;
            font-size: 0.95em;
            table-layout: fixed;
        }
        
        th {
            background: #667eea;
            color: white;
            padding: 15px 8px;
            font-weight: 600;
            text-transform: uppercase;
            letter-spacing: 0.5px;
            font-size: 0.85em;
        }
        
        th:first-child {
            border-top-left-radius: 12px;
        }
        
        th:last-child {
            border-top-right-radius: 12px;
        }
        
        td {
            padding: 12px 6px;
            border-bottom: 1px solid #e9ecef;
            color: #495057;
            vertical-align: middle;
            word-wrap: break-word;
        }
        
        /* Column-specific alignments */
        th:nth-child(1), td:nth-child(1) { /* Name Column */
            text-align: left;
            width: 20%;
        }
        
        th:nth-child(2), td:nth-child(2) { /* Grade-Section Column */
            text-align: center;
            width: 12%;
        }
        
        th:nth-child(3), td:nth-child(3) { /* Location Column */
            text-align: center;
            width: 15%;
        }
        
        th:nth-child(4), td:nth-child(4) { /* Time In Column */
            text-align: center;
            width: 12%;
        }
        
        th:nth-child(5), td:nth-child(5) { /* Time Out Column */
            text-align: center;
            width: 12%;
        }
        
        th:nth-child(6), td:nth-child(6) { /* Remark Column */
            text-align: center;
            width: 12%;
        }
        
        tr:hover {
            background-color: #f8f9fa;
            transition: background-color 0.2s;
        }
        
        tr:last-child td {
            border-bottom: none;
        }
        
        .delete-btn {
            background: #e74c3c;
            color: white;
            border: none;
            padding: 6px 12px;
            border-radius: 6px;
            cursor: pointer;
            font-size: 0.85em;
            font-weight: 500;
            transition: all 0.2s;
            box-shadow: 0 2px 4px rgba(231,76,60,0.2);
        }
        
        .delete-btn:hover {
            background: #c0392b;
            transform: translateY(-1px);
            box-shadow: 0 4px 8px rgba(231,76,60,0.3);
        }
        
        .toast {
            position: fixed;
            top: 20px;
            right: 20px;
            background: #27ae60;
            color: white;
            padding: 15px 25px;
            border-radius: 10px;
            animation: slideIn 0.3s ease-out;
            z-index: 1000;
            box-shadow: 0 5px 15px rgba(0,0,0,0.2);
            font-weight: 500;
        }
        
        @keyframes slideIn {
            from { transform: translateX(100%); opacity: 0; }
            to { transform: translateX(0); opacity: 1; }
        }
        
        .loading {
            display: inline-block;
            width: 50px;
            height: 50px;
            border: 4px solid #f3f3f3;
            border-top: 4px solid #667eea;
            border-radius: 50%;
            animation: spin 1s linear infinite;
            margin: 20px auto;
        }
        
        @keyframes spin {
            0% { transform: rotate(0deg); }
            100% { transform: rotate(360deg); }
        }
        
        .badge {
            display: inline-block;
            padding: 4px 8px;
            border-radius: 20px;
            font-size: 0.8em;
            font-weight: 600;
            text-transform: uppercase;
            letter-spacing: 0.5px;
            min-width: 80px;
            text-align: center;
        }
        
        .badge-admission { background: #3498db; color: white; }
        .badge-registrar { background: #e67e22; color: white; }
        .badge-cr { background: #27ae60; color: white; }
        .badge-cashier { background: #9b59b6; color: white; }
        .badge-canteen { background: #f1c40f; color: #2c3e50; }
        .badge-library { background: #1abc9c; color: white; }
        .badge-clinic { background: #e74c3c; color: white; }
        .badge-guidance { background: #8e44ad; color: white; }
        
        .badge-ontime { background: #27ae60; color: white; }
        .badge-late { background: #e74c3c; color: white; }
        .badge-early { background: #f39c12; color: white; }
        .badge-overtime { background: #8e44ad; color: white; }
        .badge-normal { background: #7f8c8d; color: white; }
        
        .badge-active { background: #27ae60; color: white; }
        .badge-inactive { background: #7f8c8d; color: white; }
        
        .filters {
            display: flex;
            gap: 10px;
            margin: 20px 0;
            flex-wrap: wrap;
        }
        
        .filter-btn {
            padding: 8px 18px;
            border: 1px solid #dee2e6;
            background: white;
            border-radius: 25px;
            cursor: pointer;
            transition: all 0.2s;
            font-size: 0.9em;
            font-weight: 500;
        }
        
        .filter-btn:hover {
            background: #e9ecef;
            border-color: #adb5bd;
        }
        
        .filter-btn.active {
            background: #667eea;
            color: white;
            border-color: #667eea;
            box-shadow: 0 2px 8px rgba(102,126,234,0.3);
        }
        
        /* Time Configuration Panel */
        .config-panel {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 25px;
            border-radius: 15px;
            margin: 20px 0;
            box-shadow: 0 10px 20px rgba(102,126,234,0.3);
        }
        
        .config-title {
            font-size: 1.3em;
            font-weight: 600;
            margin-bottom: 20px;
            display: flex;
            align-items: center;
            gap: 10px;
        }
        
        .config-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 20px;
            margin-bottom: 20px;
        }
        
        .config-item {
            background: rgba(255,255,255,0.1);
            padding: 15px;
            border-radius: 10px;
        }
        
        .config-label {
            font-size: 0.9em;
            opacity: 0.9;
            margin-bottom: 5px;
        }
        
        .config-value {
            font-size: 1.2em;
            font-weight: 600;
        }
        
        .config-input {
            width: 100%;
            padding: 10px;
            border: none;
            border-radius: 8px;
            font-size: 1em;
            margin-top: 5px;
        }
        
        .config-checkbox {
            width: 20px;
            height: 20px;
            margin-right: 10px;
        }
        
        .debug-panel {
            background: #f8f9fa;
            border: 1px solid #dee2e6;
            border-radius: 12px;
            padding: 20px;
            margin: 25px 0;
            font-family: 'Courier New', monospace;
            font-size: 0.9em;
            max-height: 400px;
            overflow-y: auto;
        }
        
        .capacity-bar {
            width: 100%;
            height: 25px;
            background: #e9ecef;
            border-radius: 15px;
            margin: 15px 0;
            overflow: hidden;
            box-shadow: inset 0 1px 3px rgba(0,0,0,0.1);
        }
        
        .capacity-fill {
            height: 100%;
            background: linear-gradient(90deg, #27ae60, #f39c12, #e74c3c);
            transition: width 0.3s ease;
            border-radius: 15px;
        }
        
        .warning-box {
            background: #fff3cd;
            border: 1px solid #ffeeba;
            color: #856404;
            padding: 15px 20px;
            border-radius: 10px;
            margin: 20px 0;
            font-weight: 500;
        }
        
        .weather-info {
            display: flex;
            align-items: center;
            justify-content: flex-end;
            gap: 10px;
            margin-top: 20px;
            padding-top: 20px;
            border-top: 1px solid #e9ecef;
            color: #6c757d;
            font-size: 0.95em;
        }
        
        /* Form input styles */
        input[type="text"], input[type="number"], input[type="time"], input[type="datetime-local"] {
            width: 100%;
            padding: 12px 14px;
            margin: 8px 0;
            border: 2px solid #e9ecef;
            border-radius: 10px;
            font-size: 1em;
            transition: border-color 0.3s;
        }
        
        input[type="text"]:focus, input[type="number"]:focus, input[type="time"]:focus, input[type="datetime-local"]:focus {
            outline: none;
            border-color: #667eea;
            box-shadow: 0 0 0 3px rgba(102,126,234,0.1);
        }
        
        .section-title {
            font-size: 1.3em;
            color: #495057;
            margin: 20px 0 15px 0;
            font-weight: 600;
            display: flex;
            align-items: center;
            gap: 10px;
        }
        
        .duration-badge {
            background: #e9ecef;
            padding: 3px 8px;
            border-radius: 15px;
            font-size: 0.85em;
            font-weight: 500;
        }
        
        .clickable {
            cursor: pointer;
            transition: background-color 0.2s;
        }
        
        .clickable:hover {
            background-color: #f0f0f0;
        }
        
        /* Responsive design */
        @media (max-width: 1024px) {
            .stats {
                grid-template-columns: repeat(2, 1fr);
            }
            
            .menu {
                grid-template-columns: repeat(2, 1fr);
            }
            
            th, td {
                padding: 8px 4px;
                font-size: 0.8em;
            }
            
            .badge {
                min-width: 60px;
                padding: 3px 6px;
                font-size: 0.75em;
            }
        }
        
        @media (max-width: 768px) {
            .container {
                padding: 15px;
            }
            
            .stats {
                grid-template-columns: 1fr;
            }
            
            .menu {
                grid-template-columns: 1fr;
            }
            
            th, td {
                padding: 6px 3px;
                font-size: 0.7em;
            }
            
            .badge {
                min-width: 50px;
                padding: 2px 4px;
                font-size: 0.7em;
            }
            
            .delete-btn {
                padding: 4px 8px;
                font-size: 0.7em;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>CFSI Monitoring System</h1>
        
        <div class="stats">
            <div class="stat-card" onclick="showFilteredStudents('all')">
                <div class="stat-number" id="totalStudents">0</div>
                <div class="stat-label">Total Students</div>
            </div>
            <div class="stat-card" onclick="showFilteredStudents('today')">
                <div class="stat-number" id="todayAttendance">0</div>
                <div class="stat-label">Today's Attendance</div>
            </div>
            <div class="stat-card" onclick="showFilteredStudents('late')">
                <div class="stat-number" id="lateToday">0</div>
                <div class="stat-label">Late Today</div>
            </div>
            <div class="stat-card" onclick="showFilteredStudents('active')">
                <div class="stat-number" id="activeNow">0</div>
                <div class="stat-label">Currently Inside</div>
            </div>
        </div>
        
        <div class="menu">
            <button class="btn" onclick="showRegister()"><span>ðŸ“</span> Register</button>
            <button class="btn" onclick="showLocationSelector()"><span>ðŸ‘†</span> Scan</button>
            <button class="btn" onclick="showStudents()"><span>ðŸ“‹</span> Students</button>
            <button class="btn" onclick="showAttendance()"><span>ðŸ“Š</span> Records</button>
            <button class="btn" onclick="showReports()"><span>ðŸ“ˆ</span> Reports</button>
            <button class="btn" onclick="showTimeConfig()"><span>âš™ï¸</span> Time Config</button>
            <button class="btn" onclick="showDebug()"><span>ðŸ”§</span> Debug</button>
            <button class="btn" onclick="exportData()"><span>ðŸ“¥</span> Export</button>
            <button class="btn" onclick="showAssistant()"><span>ðŸ¤–</span> Assistant</button>
        </div>
        
        <div id="content"></div>
        
        <div class="weather-info">
            <span>ðŸŒ¡ï¸ 30Â°C</span>
            <span>â˜ï¸ Mostly cloudy</span>
        </div>
    </div>

    <script>
        let scanInterval;
        let registerInterval;
        let currentFilter = 'all';
        let selectedLocation = 0;
        
        function updateStats() {
            fetch('/api/stats')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('totalStudents').innerHTML = d.totalStudents;
                    document.getElementById('todayAttendance').innerHTML = d.todayAttendance;
                    document.getElementById('lateToday').innerHTML = d.lateToday;
                    document.getElementById('activeNow').innerHTML = d.activeNow;
                })
                .catch(e => console.log('Stats error:', e));
        }
        
        function showTimeConfig() {
            fetch('/api/time-config')
                .then(r => r.json())
                .then(d => {
                    let startHour = d.startHour.toString().padStart(2,'0');
                    let startMinute = d.startMinute.toString().padStart(2,'0');
                    let endHour = d.endHour.toString().padStart(2,'0');
                    let endMinute = d.endMinute.toString().padStart(2,'0');
                    
                    let html = `
                        <div class="config-panel">
                            <div class="config-title">âš™ï¸ Time Configuration</div>
                            <div class="config-grid">
                                <div class="config-item">
                                    <div class="config-label">Start Time</div>
                                    <input type="time" id="startTime" class="config-input" value="${startHour}:${startMinute}">
                                </div>
                                <div class="config-item">
                                    <div class="config-label">End Time</div>
                                    <input type="time" id="endTime" class="config-input" value="${endHour}:${endMinute}">
                                </div>
                                <div class="config-item">
                                    <div class="config-label">Grace Period (minutes)</div>
                                    <input type="number" id="gracePeriod" class="config-input" value="${d.gracePeriod}" min="0" max="120">
                                </div>
                                <div class="config-item">
                                    <div class="config-label">
                                        <input type="checkbox" id="autoDetect" class="config-checkbox" ${d.autoDetect ? 'checked' : ''}>
                                        Auto-detect Time In/Out
                                    </div>
                                    <div style="font-size:0.9em; margin-top:10px; opacity:0.8;">
                                        When enabled: Before start time = Time In, After end time = Time Out
                                    </div>
                                </div>
                            </div>
                            <div style="display:flex; gap:10px; margin-top:20px;">
                                <button class="btn btn-success" onclick="saveTimeConfig()" style="flex:1;">Save Configuration</button>
                                <button class="btn btn-danger" onclick="cancelForm()" style="flex:1;">Cancel</button>
                            </div>
                        </div>
                    `;
                    document.getElementById('content').innerHTML = html;
                });
        }
        
        function saveTimeConfig() {
            let startTime = document.getElementById('startTime').value.split(':');
            let endTime = document.getElementById('endTime').value.split(':');
            let gracePeriod = document.getElementById('gracePeriod').value;
            let autoDetect = document.getElementById('autoDetect').checked;
            
            fetch('/api/save-time-config', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: `startHour=${startTime[0]}&startMinute=${startTime[1]}&endHour=${endTime[0]}&endMinute=${endTime[1]}&gracePeriod=${gracePeriod}&autoDetect=${autoDetect}`
            })
            .then(r => r.json())
            .then(d => {
                if(d.success) {
                    showToast('âœ… Time configuration saved');
                    location.reload();
                } else {
                    showToast('âŒ Failed to save', 'error');
                }
            });
        }
        
        function exportData() {
            fetch('/api/export-attendance')
                .then(r => r.json())
                .then(d => {
                    let csv = "Name,Grade-Section,Location,Date,Time In,Time Out,Duration (min),Remark,Status\n";
                    d.records.forEach(r => {
                        csv += `"${r.name}","${r.grade}-${r.section}","${r.location}","${r.date}","${r.timeIn}","${r.timeOut || '---'}","${r.duration || 0}","${r.remark}","${r.isActive ? 'Inside' : 'Out'}"\n`;
                    });
                    
                    let blob = new Blob([csv], {type: 'text/csv'});
                    let url = window.URL.createObjectURL(blob);
                    let a = document.createElement('a');
                    a.href = url;
                    a.download = 'attendance_' + new Date().toISOString().slice(0,10) + '.csv';
                    a.click();
                    
                    showToast('âœ… Data exported to CSV');
                });
        }
        
        function showDebug() {
            // Get current time first
            fetch('/api/current-time')
                .then(r => r.json())
                .then(timeData => {
                    let currentDateTime = timeData.date.replace(/\//g, '-') + 'T' + timeData.hour24.toString().padStart(2,'0') + ':' + timeData.minute.toString().padStart(2,'0');
                    
                    document.getElementById('content').innerHTML = `
                        <div class="scan-area">
                            <h3>ðŸ”§ Debug Panel</h3>
                            <button class="btn" onclick="testSensor()" style="margin:5px;">Test Sensor</button>
                            <button class="btn" onclick="checkCapacity()" style="margin:5px;">Check Capacity</button>
                            <button class="btn" onclick="checkState()" style="margin:5px;">Check System State</button>
                            <button class="btn" onclick="checkRTC()" style="margin:5px;">Check RTC Time</button>
                            <button class="btn" onclick="syncNTP()" style="margin:5px;">Sync with NTP</button>
                            <button class="btn" onclick="forceTimeSync()" style="margin:5px; background:#3498db;">ðŸ•’ Force Time Sync</button>
                            <button class="btn" onclick="clearActiveSessions()" style="margin:5px;">Clear Active Sessions</button>
                            <button class="btn btn-danger" onclick="clearAllData()" style="margin:5px;">Clear ALL Data</button>
                            <button class="btn btn-warning" onclick="clearFingerprints()" style="margin:5px;">Clear Fingerprints Only</button>
                            <button class="btn" onclick="listFingerprints()" style="margin:5px;">List Fingerprints</button>
                            <button class="btn" onclick="checkConsistency()" style="margin:5px;">Check Consistency</button>
                            <button class="btn" onclick="checkRegistrationState()" style="margin:5px;">Check Registration State</button>
                            
                            <h4 style="margin-top:20px;">ðŸ•’ Manual Time Set</h4>
                            <input type="datetime-local" id="manualDateTime" value="${currentDateTime}" style="width:100%; margin:10px 0;">
                            <button class="btn btn-success" onclick="setManualTime()" style="width:100%;">Set Manual Time</button>
                            
                            <button class="btn btn-reset" onclick="resetEEPROM()" style="margin-top:20px; background:#e74c3c;">âš ï¸ RESET EEPROM (Factory Reset)</button>
                            <div id="debugOutput" class="debug-panel">Click a button to test...</div>
                        </div>
                    `;
                });
        }
        
        let chatHistory = [];
        const assistantModeNames = ['ðŸ”’ Offline', 'â˜ï¸ Cloud AI', 'ðŸ–¥ï¸ Local AI'];

        function showAssistant() {
            chatHistory = [
                { from: 'bot', text: "Hi! I'm the CFSI Assistant. Ask me about attendance â€” try one of the questions below, or type your own." }
            ];

            document.getElementById('content').innerHTML = `
                <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:15px;">
                    <h3 style="margin:0;">ðŸ¤– Assistant <span id="assistantModeBadge" style="font-size:0.5em; background:#e9ecef; padding:4px 10px; border-radius:12px; vertical-align:middle;"></span></h3>
                    <button class="btn" style="width:auto; padding:8px 15px;" onclick="showAssistantSettings()">âš™ï¸ Settings</button>
                </div>
                <div class="chat-container">
                    <div class="chat-messages" id="chatMessages"></div>
                    <div class="chat-suggestions">
                        <div class="chat-chip" onclick="askSuggested('Who is late today?')">Who's late today?</div>
                        <div class="chat-chip" onclick="askSuggested('Who is inside right now?')">Who's inside now?</div>
                        <div class="chat-chip" onclick="askSuggested('How many students today?')">Attendance today</div>
                        <div class="chat-chip" onclick="askSuggested('Total students')">Total students</div>
                        <div class="chat-chip" onclick="askSuggested('Who is absent today?')">Who's absent?</div>
                        <div class="chat-chip" onclick="askSuggested('help')">What can you do?</div>
                    </div>
                    <div class="chat-input-row">
                        <input type="text" id="chatInput" placeholder="Ask about attendance..." onkeypress="if(event.key==='Enter') sendAssistantMsg()">
                        <button class="chat-send-btn" onclick="sendAssistantMsg()">Send</button>
                    </div>
                </div>
            `;

            renderChat();

            fetch('/api/assistant-config')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('assistantModeBadge').innerText = assistantModeNames[d.mode] || 'Offline';
                });
        }

        function showAssistantSettings() {
            fetch('/api/assistant-config')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('content').innerHTML = `
                        <h3 style="margin-bottom:15px;">âš™ï¸ Assistant Settings</h3>
                        <div class="scan-area" style="text-align:left;">
                            <label style="font-weight:600;">Mode</label>
                            <select id="cfgMode" style="width:100%; padding:10px; margin:8px 0 20px 0; border-radius:8px; border:2px solid #e9ecef;" onchange="toggleAssistantModeFields()">
                                <option value="0" ${d.mode==0?'selected':''}>ðŸ”’ Offline (built-in rules, no network needed)</option>
                                <option value="1" ${d.mode==1?'selected':''}>â˜ï¸ Cloud AI (real LLM, needs internet)</option>
                                <option value="2" ${d.mode==2?'selected':''}>ðŸ–¥ï¸ Local AI (real LLM on a Pi/PC on your network)</option>
                            </select>

                            <div id="cloudFields" style="display:${d.mode==1?'block':'none'};">
                                <h4>â˜ï¸ Cloud AI (Anthropic API)</h4>
                                <p style="color:#6c757d; font-size:0.85em;">The ESP32 will also join this router WiFi (in addition to its own self-hosted network) so it can reach the internet.</p>
                                <label>Router WiFi Name (SSID)</label>
                                <input type="text" id="cfgWifiSSID" value="${d.wifiSSID}" style="width:100%; padding:10px; margin:6px 0 14px 0; border-radius:8px; border:2px solid #e9ecef;">
                                <label>Router WiFi Password</label>
                                <input type="password" id="cfgWifiPassword" placeholder="${d.hasWifiPassword ? 'â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢ (saved - leave blank to keep)' : 'Enter password'}" style="width:100%; padding:10px; margin:6px 0 14px 0; border-radius:8px; border:2px solid #e9ecef;">
                                <label>Anthropic API Key</label>
                                <input type="password" id="cfgApiKey" placeholder="${d.hasApiKey ? 'â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢ (saved - leave blank to keep)' : 'sk-ant-...'}" style="width:100%; padding:10px; margin:6px 0 14px 0; border-radius:8px; border:2px solid #e9ecef;">
                                <label>Model</label>
                                <input type="text" id="cfgApiModel" value="${d.apiModel}" style="width:100%; padding:10px; margin:6px 0 14px 0; border-radius:8px; border:2px solid #e9ecef;">
                                <p style="font-size:0.8em; color:${d.wifiConnected?'#27ae60':'#e74c3c'};">${d.wifiConnected ? 'âœ… Currently connected to the internet' : 'âš ï¸ Not currently connected to the internet'}</p>
                            </div>

                            <div id="localFields" style="display:${d.mode==2?'block':'none'};">
                                <h4>ðŸ–¥ï¸ Local AI (e.g. Ollama on a Raspberry Pi/PC)</h4>
                                <p style="color:#6c757d; font-size:0.85em;">That device should be connected to this ESP32's own WiFi network ("CFSI_Monitoring") and running an LLM server. No internet needed.</p>
                                <label>Server Address</label>
                                <input type="text" id="cfgLocalURL" value="${d.localURL}" placeholder="http://192.168.4.50:11434" style="width:100%; padding:10px; margin:6px 0 14px 0; border-radius:8px; border:2px solid #e9ecef;">
                                <label>Model Name</label>
                                <input type="text" id="cfgLocalModel" value="${d.localModel}" style="width:100%; padding:10px; margin:6px 0 14px 0; border-radius:8px; border:2px solid #e9ecef;">
                            </div>

                            <button class="btn btn-success" style="margin-top:10px;" onclick="saveAssistantConfig()">ðŸ’¾ Save Settings</button>
                            <button class="btn" style="margin-top:10px;" onclick="showAssistant()">â† Back to Assistant</button>
                        </div>
                    `;
                });
        }

        function toggleAssistantModeFields() {
            let mode = document.getElementById('cfgMode').value;
            document.getElementById('cloudFields').style.display = (mode == '1') ? 'block' : 'none';
            document.getElementById('localFields').style.display = (mode == '2') ? 'block' : 'none';
        }

        function saveAssistantConfig() {
            let mode = document.getElementById('cfgMode').value;
            let params = `mode=${mode}`;

            if (mode == '1') {
                params += `&wifiSSID=${encodeURIComponent(document.getElementById('cfgWifiSSID').value)}`;
                params += `&wifiPassword=${encodeURIComponent(document.getElementById('cfgWifiPassword').value)}`;
                params += `&apiKey=${encodeURIComponent(document.getElementById('cfgApiKey').value)}`;
                params += `&apiModel=${encodeURIComponent(document.getElementById('cfgApiModel').value)}`;
            } else if (mode == '2') {
                params += `&localURL=${encodeURIComponent(document.getElementById('cfgLocalURL').value)}`;
                params += `&localModel=${encodeURIComponent(document.getElementById('cfgLocalModel').value)}`;
            }

            fetch('/api/save-assistant-config', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: params
            })
            .then(r => r.json())
            .then(d => {
                if (d.success) {
                    showToast('âœ… Assistant settings saved');
                    showAssistant();
                } else {
                    showToast('âŒ Failed to save settings', 'error');
                }
            })
            .catch(e => {
                showToast('âŒ Connection error', 'error');
                console.log(e);
            });
        }

        function askSuggested(q) {
            document.getElementById('chatInput').value = q;
            sendAssistantMsg();
        }

        function renderChat() {
            let el = document.getElementById('chatMessages');
            if (!el) return;
            el.innerHTML = chatHistory.map(m =>
                `<div class="chat-msg ${m.from === 'user' ? 'chat-msg-user' : 'chat-msg-bot'}">${m.text}</div>`
            ).join('');
            el.scrollTop = el.scrollHeight;
        }

        function sendAssistantMsg() {
            let input = document.getElementById('chatInput');
            let query = input.value.trim();
            if (!query) return;

            chatHistory.push({ from: 'user', text: query });
            input.value = '';
            renderChat();

            fetch('/api/assistant', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: `query=${encodeURIComponent(query)}`
            })
            .then(r => r.json())
            .then(d => {
                chatHistory.push({ from: 'bot', text: d.answer || "Sorry, I couldn't process that." });
                renderChat();
            })
            .catch(e => {
                chatHistory.push({ from: 'bot', text: "âš ï¸ Connection error â€” couldn't reach the device." });
                renderChat();
                console.log(e);
            });
        }

        function forceTimeSync() {
            document.getElementById('debugOutput').innerHTML = 'Force syncing time with NTP server...';
            fetch('/api/sync-time')
                .then(r => r.json())
                .then(d => {
                    if(d.success) {
                        document.getElementById('debugOutput').innerHTML = 
                            'âœ… ' + d.message + '<br>Date: ' + d.date + '<br>Time: ' + d.time;
                        showToast('âœ… Time synced: ' + d.time);
                    } else {
                        document.getElementById('debugOutput').innerHTML = 'âŒ ' + d.message;
                    }
                });
        }
        
        function setManualTime() {
            let dateTimeStr = document.getElementById('manualDateTime').value;
            if (!dateTimeStr) {
                showToast('âŒ Please select date and time', 'error');
                return;
            }
            
            let date = new Date(dateTimeStr);
            let year = date.getFullYear();
            let month = date.getMonth() + 1;
            let day = date.getDate();
            let hour = date.getHours();
            let minute = date.getMinutes();
            let second = 0;
            
            fetch('/api/set-time', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: `year=${year}&month=${month}&day=${day}&hour=${hour}&minute=${minute}&second=${second}`
            })
            .then(r => r.json())
            .then(d => {
                if(d.success) {
                    showToast('âœ… Time set manually');
                    document.getElementById('debugOutput').innerHTML = 'âœ… Time set to: ' + dateTimeStr;
                } else {
                    showToast('âŒ Failed to set time', 'error');
                }
            });
        }
        
        function clearActiveSessions() {
            if(confirm('Clear all active sessions? This will force check out all students.')) {
                fetch('/api/clear-active-sessions')
                    .then(r => r.json())
                    .then(d => {
                        if(d.success) {
                            showToast('âœ… Active sessions cleared');
                            document.getElementById('debugOutput').innerHTML = 'Cleared ' + d.cleared + ' active sessions';
                        }
                    });
            }
        }
        
        function checkRTC() {
            fetch('/api/check-rtc')
                .then(r => r.json())
                .then(d => {
                    let html = '<h4>â° RTC Time:</h4>';
                    html += 'Date: ' + d.date + '<br>';
                    html += 'Time: ' + d.time + '<br>';
                    html += 'Valid: ' + (d.valid ? 'âœ…' : 'âŒ') + '<br>';
                    html += '<br><small>Philippines Time (UTC+8)</small>';
                    document.getElementById('debugOutput').innerHTML = html;
                });
        }
        
        function syncNTP() {
            document.getElementById('debugOutput').innerHTML = 'Syncing with NTP...';
            fetch('/api/sync-ntp')
                .then(r => r.json())
                .then(d => {
                    if(d.success) {
                        document.getElementById('debugOutput').innerHTML = 'âœ… ' + d.message + '<br>New time: ' + d.time;
                    } else {
                        document.getElementById('debugOutput').innerHTML = 'âŒ ' + d.message;
                    }
                });
        }
        
        function resetEEPROM() {
            if(confirm('âš ï¸âš ï¸âš ï¸ THIS WILL DELETE ALL STUDENTS! Are you ABSOLUTELY sure?')) {
                if(confirm('LAST WARNING! All student data will be lost forever. Continue?')) {
                    document.getElementById('debugOutput').innerHTML = 'Resetting EEPROM to factory defaults...';
                    fetch('/api/reset-eeprom')
                        .then(r => r.json())
                        .then(d => {
                            if(d.success) {
                                document.getElementById('debugOutput').innerHTML = 'âœ… ' + d.message;
                                showToast('âœ… EEPROM reset complete');
                                updateStats();
                            } else {
                                document.getElementById('debugOutput').innerHTML = 'âŒ ' + d.message;
                            }
                        })
                        .catch(e => {
                            document.getElementById('debugOutput').innerHTML = 'âŒ Error: ' + e;
                        });
                }
            }
        }
        
        function checkState() {
            fetch('/api/debug-state')
                .then(r => r.json())
                .then(d => {
                    let html = '<h4>ðŸ“Š System State:</h4>';
                    html += 'Registration Mode: ' + (d.registrationMode ? 'âœ… Active' : 'âŒ Inactive') + '<br>';
                    html += 'Registration Step: ' + d.registrationStep + '<br>';
                    html += 'Pending ID: ' + d.pendingId + '<br>';
                    html += 'Last Success ID: ' + d.lastSuccessId + '<br>';
                    html += 'Student Count: ' + d.studentCount + '<br>';
                    html += 'Attendance Count: ' + d.attendanceCount + '<br>';
                    document.getElementById('debugOutput').innerHTML = html;
                });
        }
        
        function checkCapacity() {
            fetch('/api/sensor-capacity')
                .then(r => r.json())
                .then(d => {
                    let percent = (d.current / d.max * 100).toFixed(1);
                    let color = percent < 50 ? '#27ae60' : (percent < 80 ? '#f39c12' : '#e74c3c');
                    let html = '<h4>ðŸ“Š Sensor Capacity:</h4>';
                    html += 'Current: ' + d.current + ' / ' + d.max + '<br>';
                    html += '<div class="capacity-bar"><div class="capacity-fill" style="width:' + percent + '%; background:' + color + ';"></div></div>';
                    html += 'Used: ' + percent + '%<br>';
                    if(d.current >= d.max - 10) {
                        html += '<p style="color:#e74c3c;">âš ï¸ Sensor almost full! Delete some fingerprints.</p>';
                    }
                    document.getElementById('debugOutput').innerHTML = html;
                });
        }
        
        function checkRegistrationState() {
            fetch('/api/registration-state')
                .then(r => r.json())
                .then(d => {
                    let html = '<h4>ðŸ“‹ Registration State:</h4>';
                    html += 'Registration Mode: ' + (d.registrationMode ? 'âœ… Active' : 'âŒ Inactive') + '<br>';
                    html += 'Registration Step: ' + d.registrationStep + '<br>';
                    html += 'Pending ID: ' + d.pendingId + '<br>';
                    html += 'Last Success ID: ' + d.lastSuccessId + '<br>';
                    document.getElementById('debugOutput').innerHTML = html;
                });
        }
        
        function testSensor() {
            document.getElementById('debugOutput').innerHTML = 'Testing sensor...';
            fetch('/api/test-sensor')
                .then(r => r.json())
                .then(d => {
                    if(d.success) {
                        document.getElementById('debugOutput').innerHTML = 
                            'âœ… Sensor OK<br>Templates: ' + d.templates + '<br>Confidence: ' + d.confidence;
                    } else {
                        document.getElementById('debugOutput').innerHTML = 
                            'âŒ ' + d.message;
                    }
                })
                .catch(e => {
                    document.getElementById('debugOutput').innerHTML = 'âŒ Error: ' + e;
                });
        }
        
        function clearAllData() {
            if(confirm('âš ï¸ This will delete ALL students and ALL fingerprints! This cannot be undone!')) {
                document.getElementById('debugOutput').innerHTML = 'Clearing all data...';
                fetch('/api/clear-all-data')
                    .then(r => r.json())
                    .then(d => {
                        if(d.success) {
                            document.getElementById('debugOutput').innerHTML = 'âœ… ' + d.message;
                            showToast('âœ… All data cleared');
                            updateStats();
                        } else {
                            document.getElementById('debugOutput').innerHTML = 'âŒ ' + d.message;
                        }
                    });
            }
        }
        
        function clearFingerprints() {
            if(confirm('Delete ALL fingerprints from sensor? This cannot be undone!')) {
                document.getElementById('debugOutput').innerHTML = 'Deleting fingerprints...';
                fetch('/api/clear-fingerprints')
                    .then(r => r.json())
                    .then(d => {
                        if(d.success) {
                            document.getElementById('debugOutput').innerHTML = 'âœ… ' + d.message;
                            showToast('âœ… Fingerprints cleared');
                        } else {
                            document.getElementById('debugOutput').innerHTML = 'âŒ ' + d.message;
                        }
                        listFingerprints();
                    });
            }
        }
        
        function listFingerprints() {
            fetch('/api/list-fingerprints')
                .then(r => r.json())
                .then(d => {
                    let html = '<h4>ðŸ“‹ Stored Fingerprint IDs:</h4>';
                    if(d.ids && d.ids.length > 0) {
                        html += '<div class="table-container"><table><tr><th>ID</th><th>In Database</th><th>Action</th></tr>';
                        d.ids.forEach((id, index) => {
                            let inDB = d.inDatabase && d.inDatabase[index] ? 'âœ…' : 'âŒ';
                            html += '<tr>' +
                                '<td style="text-align:center;">' + id + '</td>' +
                                '<td style="text-align:center;">' + inDB + '</td>' +
                                '<td style="text-align:center;"><button class="delete-btn" onclick="deleteFingerprint(' + id + ')">Delete</button></td>' +
                                '</tr>';
                        });
                        html += '</table></div>';
                    } else {
                        html += 'No fingerprints stored in sensor';
                    }
                    document.getElementById('debugOutput').innerHTML = html;
                });
        }
        
        function checkConsistency() {
            fetch('/api/check-consistency')
                .then(r => r.json())
                .then(d => {
                    let html = '<h4>ðŸ“Š Consistency Check:</h4>';
                    html += 'Students in database: ' + d.studentsInDB + '<br>';
                    html += 'Fingerprints in sensor: ' + d.fingerprintsInSensor + '<br>';
                    html += 'Matching: ' + d.matching + '<br>';
                    html += 'Orphaned fingerprints: ' + d.orphaned + '<br>';
                    html += 'Missing in sensor: ' + d.missing + '<br>';
                    
                    if(d.orphaned > 0) {
                        html += '<button class="btn btn-warning" onclick="cleanOrphaned()" style="margin-top:10px;">Clean Orphaned</button>';
                    }
                    
                    document.getElementById('debugOutput').innerHTML = html;
                });
        }
        
        function cleanOrphaned() {
            if(confirm('Delete fingerprints that have no matching student?')) {
                fetch('/api/clean-orphaned')
                    .then(r => r.json())
                    .then(d => {
                        if(d.success) {
                            showToast('âœ… Orphaned fingerprints cleaned');
                            checkConsistency();
                        }
                    });
            }
        }
        
        function deleteFingerprint(id) {
            if(confirm('Delete fingerprint ID ' + id + '?')) {
                fetch('/api/delete-fingerprint', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                    body: 'id=' + id
                })
                .then(r => r.json())
                .then(d => {
                    if(d.success) {
                        showToast('âœ… ' + d.message);
                        listFingerprints();
                    } else {
                        showToast('âŒ ' + d.message, 'error');
                    }
                });
            }
        }
        
        function showLocationSelector() {
            fetch('/api/locations')
                .then(r => r.json())
                .then(locations => {
                    let html = `
                        <div class="scan-area">
                            <h3>ðŸ“ Select Location</h3>
                            <div class="location-selector" id="locationSelector">
                    `;
                    
                    locations.forEach((loc, index) => {
                        let icon = "ðŸ¢";
                        if(index === 1) icon = "ðŸ“‹";
                        else if(index === 2) icon = "ðŸ›ï¸";
                        else if(index === 3) icon = "ðŸ’°";
                        else if(index === 4) icon = "â˜•";
                        else if(index === 5) icon = "ðŸ“š";
                        else if(index === 6) icon = "ðŸ¥";
                        else if(index === 7) icon = "ðŸ§˜";
                        
                        html += `<div class="location-btn" onclick="selectLocation(${index})">${icon} ${loc}</div>`;
                    });
                    
                    html += `
                            </div>
                            <p id="selectedLocationDisplay" style="margin:20px 0; font-weight:500;">Selected: Admission Office</p>
                            <button class="btn btn-success" onclick="startScan()" style="width:100%;">Continue to Scan</button>
                            <button class="btn btn-danger" onclick="cancelForm()" style="width:100%; margin-top:10px;">Cancel</button>
                        </div>
                    `;
                    document.getElementById('content').innerHTML = html;
                });
        }
        
        function selectLocation(loc) {
            selectedLocation = loc;
            let locationNames = ['Admission Office', 'Registrar', 'CR', 'Cashier', 'Canteen', 'Library', 'Clinic', 'Guidance Office'];
            document.querySelectorAll('.location-btn').forEach((btn, idx) => {
                if (idx === loc) {
                    btn.classList.add('selected');
                } else {
                    btn.classList.remove('selected');
                }
            });
            document.getElementById('selectedLocationDisplay').innerHTML = 'Selected: ' + locationNames[loc];
        }
        
        function showRegister() {
            document.getElementById('content').innerHTML = `
                <div class="scan-area">
                    <h3>ðŸ“ Register New Student</h3>
                    <input type="text" id="name" placeholder="Full Name" maxlength="19">
                    <input type="text" id="grade" placeholder="Grade (e.g., 12)" maxlength="4">
                    <input type="text" id="section" placeholder="Section (e.g., A-123)" maxlength="8">
                    <button class="btn" onclick="registerStudent()" style="width:100%; margin-top:15px;">Start Registration</button>
                    <button class="btn btn-danger" onclick="cancelForm()" style="width:100%; margin-top:10px;">Cancel</button>
                </div>
            `;
        }
        
        function registerStudent() {
            let name = document.getElementById('name').value.trim();
            let grade = document.getElementById('grade').value.trim();
            let section = document.getElementById('section').value.trim();
            
            if(!name || !grade || !section) {
                showToast('Please fill all fields', 'error');
                return;
            }
            
            document.getElementById('content').innerHTML = `
                <div class="scan-area">
                    <div class="fingerprint-icon">ðŸ‘†</div>
                    <div class="loading"></div>
                    <h3>Place finger on sensor to register</h3>
                    <p id="registerStatus" style="margin:15px 0; color:#667eea; font-weight:500;">Waiting for fingerprint...</p>
                    <p style="color:#6c757d;">Step 1: Place finger</p>
                    <button class="btn btn-danger" onclick="cancelRegistration()" style="margin-top:20px;">Cancel</button>
                </div>
            `;
            
            fetch('/api/register-start', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: `name=${encodeURIComponent(name)}&grade=${encodeURIComponent(grade)}&section=${encodeURIComponent(section)}`
            })
            .then(r => r.json())
            .then(d => {
                if(d.success) {
                    registerInterval = setInterval(checkRegistration, 1000);
                } else {
                    showToast('âŒ Failed to start registration', 'error');
                }
            })
            .catch(e => {
                showToast('âŒ Connection error', 'error');
                console.log(e);
            });
        }
        
        function checkRegistration() {
            fetch('/api/register-status')
                .then(r => r.json())
                .then(d => {
                    if(d.status === 'waiting') {
                        document.getElementById('registerStatus').innerHTML = 'Waiting for finger...';
                    } else if(d.status === 'scan1') {
                        document.getElementById('registerStatus').innerHTML = 'âœ… First scan complete! Remove finger';
                    } else if(d.status === 'scan2') {
                        document.getElementById('registerStatus').innerHTML = 'Place same finger again';
                    } else if(d.status === 'saving') {
                        document.getElementById('registerStatus').innerHTML = 'Saving fingerprint...';
                    } else if(d.status === 'success') {
                        clearInterval(registerInterval);
                        document.getElementById('content').innerHTML = `
                            <div class="scan-area">
                                <h3 style="color:#27ae60; font-size:1.5em;">âœ… Registration Successful!</h3>
                                <p style="margin:15px 0; font-size:1.1em;"><strong>Student:</strong> ${d.name}</p>
                                <p style="margin:10px 0;"><strong>ID:</strong> ${d.id}</p>
                                <p style="margin:10px 0;"><strong>Grade & Section:</strong> ${d.grade}-${d.section}</p>
                                <button class="btn" onclick="location.reload()" style="margin-top:15px;">OK</button>
                            </div>
                        `;
                        showToast('âœ… Registered! ID: ' + d.id);
                        updateStats();
                    } else if(d.status === 'failed') {
                        clearInterval(registerInterval);
                        document.getElementById('content').innerHTML = `
                            <div class="scan-area">
                                <h3 style="color:#e74c3c;">âŒ Registration Failed</h3>
                                <p style="margin:15px 0;">${d.message}</p>
                                <button class="btn" onclick="showRegister()">Try Again</button>
                            </div>
                        `;
                        showToast('âŒ Registration failed', 'error');
                    }
                })
                .catch(e => console.log('Check error:', e));
        }
        
        function cancelRegistration() {
            clearInterval(registerInterval);
            fetch('/api/register-cancel')
                .then(() => location.reload());
        }
        
        function startScan() {
            let locationNames = ['Admission Office', 'Registrar', 'CR', 'Cashier', 'Canteen', 'Library', 'Clinic', 'Guidance Office'];
            document.getElementById('content').innerHTML = `
                <div class="scan-area">
                    <div class="fingerprint-icon">ðŸ‘†</div>
                    <h3>Place finger for attendance</h3>
                    <p style="margin:15px 0; font-weight:500;"><strong>Location:</strong> ${locationNames[selectedLocation]}</p>
                    <p id="scanStatus" style="color:#667eea; font-weight:500;">Waiting for fingerprint...</p>
                    <button class="btn btn-danger" onclick="cancelScan()" style="margin-top:20px;">Cancel</button>
                </div>
            `;
            fetch('/api/start-scan?location=' + selectedLocation);
            scanInterval = setInterval(checkScan, 1000);
        }
        
        function checkScan() {
            fetch('/api/scan-result')
                .then(r => r.json())
                .then(d => {
                    if(d.scanned) {
                        document.getElementById('scanStatus').innerHTML = 
                            `âœ… ${d.name} (${d.grade}-${d.section})<br>${d.type} at ${d.time}<br>ðŸ“ ${d.location}<br>â±ï¸ ${d.remark}`;
                        showToast(`${d.name} - ${d.type} - ${d.remark}`);
                        updateStats();
                        setTimeout(() => {
                            document.getElementById('scanStatus').innerHTML = 'Waiting for fingerprint...';
                        }, 3000);
                    }
                })
                .catch(e => console.log('Scan error:', e));
        }
        
        function cancelScan() {
            clearInterval(scanInterval);
            fetch('/api/cancel-scan')
                .then(() => location.reload());
        }
        
        function showStudents() {
            fetch('/api/students')
                .then(r => r.json())
                .then(d => {
                    if(d.students && d.students.length > 0) {
                        let html = '<div class="section-title">Registered Students</div>';
                        html += '<div class="table-container">';
                        html += '<table>';
                        html += '<tr><th>ID</th><th>Name</th><th>Grade-Section</th><th>Action</th></tr>';
                        
                        d.students.forEach(s => {
                            html += `<tr>
                                <td style="text-align:center; font-weight:500;">${s.id}</td>
                                <td style="text-align:left;">${s.name}</td>
                                <td style="text-align:center;"><span style="background:#e9ecef; padding:4px 12px; border-radius:20px; font-weight:500;">${s.grade}-${s.section}</span></td>
                                <td style="text-align:center;"><button class="delete-btn" onclick="deleteStudent(${s.id})">Delete</button></td>
                            </tr>`;
                        });
                        
                        html += '</table>';
                        html += '</div>';
                        document.getElementById('content').innerHTML = html;
                    } else {
                        document.getElementById('content').innerHTML = '<div class="scan-area"><h3>No students registered yet</h3><p style="margin-top:10px;">Click "Register" to add a new student.</p></div>';
                    }
                })
                .catch(e => {
                    console.log('Students error:', e);
                    showToast('âŒ Failed to load students', 'error');
                });
        }
        
        function showAttendance() {
            fetch('/api/attendance')
                .then(r => r.json())
                .then(d => {
                    if(d.records && d.records.length > 0) {
                        let html = '<div class="section-title">Attendance Records</div>';
                        html += `
                            <div class="filters">
                                <button class="filter-btn active" onclick="filterAttendance('all')">All</button>
                                <button class="filter-btn" onclick="filterAttendance('Admission Office')">ðŸ¢ Admission</button>
                                <button class="filter-btn" onclick="filterAttendance('Registrar')">ðŸ“‹ Registrar</button>
                                <button class="filter-btn" onclick="filterAttendance('CR')">ðŸ›ï¸ CR</button>
                                <button class="filter-btn" onclick="filterAttendance('Cashier')">ðŸ’° Cashier</button>
                                <button class="filter-btn" onclick="filterAttendance('Canteen')">â˜• Canteen</button>
                                <button class="filter-btn" onclick="filterAttendance('Library')">ðŸ“š Library</button>
                                <button class="filter-btn" onclick="filterAttendance('Clinic')">ðŸ¥ Clinic</button>
                                <button class="filter-btn" onclick="filterAttendance('Guidance Office')">ðŸ§˜ Guidance</button>
                                <button class="filter-btn" onclick="filterAttendance('late')">â° Late</button>
                                <button class="filter-btn" onclick="filterAttendance('ontime')">âœ… On Time</button>
                                <button class="filter-btn" onclick="filterAttendance('early')">ðŸ”† Early</button>
                            </div>
                            <div class="table-container">
                            <table id="attendanceTable">
                                <thead>
                                    <tr>
                                        <th>Name</th>
                                        <th>Grade-Sec</th>
                                        <th>Location</th>
                                        <th>Date</th>
                                        <th>Time In</th>
                                        <th>Time Out</th>
                                        <th>Duration</th>
                                        <th>Remark</th>
                                        <th>Status</th>
                                    </tr>
                                </thead>
                                <tbody>
                        `;
                        
                        d.records.forEach(r => {
                            let badgeClass = 'badge-admission';
                            if(r.location === 'Registrar') badgeClass = 'badge-registrar';
                            else if(r.location === 'CR') badgeClass = 'badge-cr';
                            else if(r.location === 'Cashier') badgeClass = 'badge-cashier';
                            else if(r.location === 'Canteen') badgeClass = 'badge-canteen';
                            else if(r.location === 'Library') badgeClass = 'badge-library';
                            else if(r.location === 'Clinic') badgeClass = 'badge-clinic';
                            else if(r.location === 'Guidance Office') badgeClass = 'badge-guidance';
                            
                            let remarkClass = 'badge-normal';
                            if(r.remark === 'On Time') remarkClass = 'badge-ontime';
                            else if(r.remark === 'Late') remarkClass = 'badge-late';
                            else if(r.remark === 'Early') remarkClass = 'badge-early';
                            else if(r.remark === 'Overtime') remarkClass = 'badge-overtime';
                            
                            let statusClass = r.isActive ? 'badge-active' : 'badge-inactive';
                            let statusText = r.isActive ? 'Inside' : 'Out';
                            
                            // Format duration
                            let durationText = '---';
                            if (r.duration && r.duration > 0) {
                                let hours = Math.floor(r.duration / 60);
                                let minutes = r.duration % 60;
                                if (hours > 0) {
                                    durationText = hours + 'h ' + minutes + 'm';
                                } else {
                                    durationText = minutes + ' min';
                                }
                            }
                            
                            html += `<tr class="attendance-row" data-location="${r.location}" data-remark="${r.remark}">
                                <td style="text-align:left; font-weight:500;">${r.name}</td>
                                <td style="text-align:center;">${r.grade}-${r.section}</td>
                                <td style="text-align:center;"><span class="badge ${badgeClass}">${r.location}</span></td>
                                <td style="text-align:center; font-family:monospace;">${r.date}</td>
                                <td style="text-align:center; font-family:monospace; font-weight:500;">${r.timeIn}</td>
                                <td style="text-align:center; font-family:monospace; font-weight:500;">${r.timeOut || '---'}</td>
                                <td style="text-align:center;"><span class="duration-badge">${durationText}</span></td>
                                <td style="text-align:center;"><span class="badge ${remarkClass}">${r.remark}</span></td>
                                <td style="text-align:center;"><span class="badge ${statusClass}">${statusText}</span></td>
                            </tr>`;
                        });
                        
                        html += '</tbody></table></div>';
                        document.getElementById('content').innerHTML = html;
                    } else {
                        document.getElementById('content').innerHTML = '<div class="scan-area"><h3>No attendance records yet</h3></div>';
                    }
                })
                .catch(e => {
                    console.log('Attendance error:', e);
                    showToast('âŒ Failed to load attendance', 'error');
                });
        }
        
        function filterAttendance(filter) {
            document.querySelectorAll('.filter-btn').forEach(btn => btn.classList.remove('active'));
            event.target.classList.add('active');
            
            let rows = document.querySelectorAll('.attendance-row');
            rows.forEach(row => {
                if(filter === 'all') {
                    row.style.display = '';
                } else if(filter === 'late' || filter === 'ontime' || filter === 'early') {
                    let remark = row.getAttribute('data-remark');
                    let filterMap = {
                        'late': 'Late',
                        'ontime': 'On Time',
                        'early': 'Early'
                    };
                    row.style.display = remark === filterMap[filter] ? '' : 'none';
                } else {
                    row.style.display = row.getAttribute('data-location') === filter ? '' : 'none';
                }
            });
        }
        
        function showReports() {
            fetch('/api/attendance-summary')
                .then(r => r.json())
                .then(d => {
                    let html = '<div class="section-title">Attendance Summary</div>';
                    html += `
                        <div style="display:grid; grid-template-columns:repeat(2,1fr); gap:15px; margin:20px 0;">
                            <div class="stat-card" style="background:#3498db; cursor:pointer;" onclick="showFilteredStudents('today')">
                                <div class="stat-number">${d.totalToday}</div>
                                <div class="stat-label">Today's Total</div>
                            </div>
                            <div class="stat-card" style="background:#e67e22; cursor:pointer;" onclick="showFilteredStudents('active')">
                                <div class="stat-number">${d.activeNow}</div>
                                <div class="stat-label">Currently Inside</div>
                            </div>
                            <div class="stat-card" style="background:#e74c3c; cursor:pointer;" onclick="showFilteredStudents('late')">
                                <div class="stat-number">${d.lateToday}</div>
                                <div class="stat-label">Late Today</div>
                            </div>
                            <div class="stat-card" style="background:#27ae60; cursor:pointer;" onclick="showFilteredStudents('ontime')">
                                <div class="stat-number">${d.ontimeToday}</div>
                                <div class="stat-label">On Time Today</div>
                            </div>
                        </div>
                        <h4 style="margin:20px 0 10px 0;">ðŸ“ Location Summary</h4>
                        <div class="table-container">
                        <table>
                            <tr><th>Location</th><th style="text-align:center;">Today</th><th style="text-align:center;">Late</th><th style="text-align:center;">On Time</th></tr>
                    `;
                    
                    for(let loc in d.locationSummary) {
                        let count = d.locationSummary[loc];
                        let lateCount = d.locationLate[loc] || 0;
                        let ontimeCount = d.locationOntime[loc] || 0;
                        let badgeClass = 'badge-admission';
                        if(loc === 'Registrar') badgeClass = 'badge-registrar';
                        else if(loc === 'CR') badgeClass = 'badge-cr';
                        else if(loc === 'Cashier') badgeClass = 'badge-cashier';
                        else if(loc === 'Canteen') badgeClass = 'badge-canteen';
                        else if(loc === 'Library') badgeClass = 'badge-library';
                        else if(loc === 'Clinic') badgeClass = 'badge-clinic';
                        else if(loc === 'Guidance Office') badgeClass = 'badge-guidance';
                        
                        html += `<tr>
                            <td><span class="badge ${badgeClass}">${loc}</span></td>
                            <td style="text-align:center; font-weight:500; cursor:pointer;" onclick="showFilteredStudents('location', '${loc}')">${count}</td>
                            <td style="text-align:center; color:#e74c3c; font-weight:500; cursor:pointer;" onclick="showFilteredStudents('late', '${loc}')">${lateCount}</td>
                            <td style="text-align:center; color:#27ae60; font-weight:500; cursor:pointer;" onclick="showFilteredStudents('ontime', '${loc}')">${ontimeCount}</td>
                        </tr>`;
                    }
                    
                    html += '</table></div>';
                    document.getElementById('content').innerHTML = html;
                });
        }
        
        function showFilteredStudents(filter, location = '') {
            let title = '';
            let filterType = filter;
            let filterLocation = location;
            
            switch(filter) {
                case 'all':
                    title = 'All Students';
                    break;
                case 'today':
                    title = 'All Students Today';
                    break;
                case 'active':
                    title = 'Currently Inside';
                    break;
                case 'late':
                    title = location ? `Late Students - ${location}` : 'Late Students Today';
                    break;
                case 'ontime':
                    title = location ? `On Time Students - ${location}` : 'On Time Students Today';
                    break;
                case 'location':
                    title = `Students at ${location}`;
                    break;
            }
            
            fetch('/api/filtered-attendance?filter=' + filter + (location ? '&location=' + encodeURIComponent(location) : ''))
                .then(r => r.json())
                .then(d => {
                    if(d.records && d.records.length > 0) {
                        let html = `<div class="section-title">${title} <span style="font-size:0.7em; background:#e9ecef; padding:5px 10px; border-radius:20px;">${d.records.length} students</span></div>`;
                        html += '<button class="btn" onclick="showReports()" style="margin-bottom:15px; width:auto;">â† Back to Summary</button>';
                        html += '<div class="table-container">';
                        html += '<table>';
                        html += '<tr><th>Name</th><th>Grade-Section</th><th>Location</th><th>Time In</th><th>Time Out</th><th>Remark</th></tr>';
                        
                        d.records.forEach(r => {
                            let badgeClass = 'badge-admission';
                            if(r.location === 'Registrar') badgeClass = 'badge-registrar';
                            else if(r.location === 'CR') badgeClass = 'badge-cr';
                            else if(r.location === 'Cashier') badgeClass = 'badge-cashier';
                            else if(r.location === 'Canteen') badgeClass = 'badge-canteen';
                            else if(r.location === 'Library') badgeClass = 'badge-library';
                            else if(r.location === 'Clinic') badgeClass = 'badge-clinic';
                            else if(r.location === 'Guidance Office') badgeClass = 'badge-guidance';
                            
                            let remarkClass = 'badge-normal';
                            if(r.remark === 'On Time') remarkClass = 'badge-ontime';
                            else if(r.remark === 'Late') remarkClass = 'badge-late';
                            else if(r.remark === 'Early') remarkClass = 'badge-early';
                            
                            html += `<tr>
                                <td style="font-weight:500;">${r.name}</td>
                                <td style="text-align:center;">${r.grade}-${r.section}</td>
                                <td style="text-align:center;"><span class="badge ${badgeClass}">${r.location}</span></td>
                                <td style="text-align:center; font-family:monospace;">${r.timeIn}</td>
                                <td style="text-align:center; font-family:monospace;">${r.timeOut || '---'}</td>
                                <td style="text-align:center;"><span class="badge ${remarkClass}">${r.remark}</span></td>
                            </tr>`;
                        });
                        
                        html += '</table></div>';
                        document.getElementById('content').innerHTML = html;
                    } else {
                        document.getElementById('content').innerHTML = `
                            <div class="scan-area">
                                <h3>No students found</h3>
                                <p style="margin:15px 0;">No records match the selected filter.</p>
                                <button class="btn" onclick="showReports()">Back to Summary</button>
                            </div>
                        `;
                    }
                });
        }
        
        function deleteStudent(id) {
            if(confirm('Delete this student? This will also delete their fingerprint.')) {
                fetch('/api/delete-student', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                    body: `id=${id}`
                })
                .then(r => r.json())
                .then(d => {
                    if(d.success) {
                        showToast('Student deleted');
                        showStudents();
                        updateStats();
                    } else {
                        showToast('âŒ ' + d.message, 'error');
                    }
                })
                .catch(e => {
                    showToast('âŒ Delete failed', 'error');
                    console.log(e);
                });
            }
        }
        
        function cancelForm() {
            location.reload();
        }
        
        function showToast(msg, type='success') {
            let toast = document.createElement('div');
            toast.className = 'toast';
            toast.style.background = type=='success' ? '#27ae60' : '#e74c3c';
            toast.innerHTML = msg;
            document.body.appendChild(toast);
            setTimeout(() => toast.remove(), 3000);
        }
        
        // Initialize
        setInterval(updateStats, 5000);
        updateStats();
    </script>
</body>
</html>
)rawliteral";

// ============= SYNC TIME WITH NTP =============
bool syncTimeWithNTP() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("ðŸŒ Syncing time with NTP...");
    
    // Configure time with UTC+8 offset for Philippines
    configTime(utcOffsetInSeconds, 0, "pool.ntp.org", "time.nist.gov", "asia.pool.ntp.org");
    
    struct tm timeinfo;
    int retry = 0;
    while (!getLocalTime(&timeinfo) && retry < 10) {
      Serial.print(".");
      delay(1000);
      retry++;
    }
    
    if (getLocalTime(&timeinfo)) {
      // Convert to DateTime and set RTC
      DateTime ntpTime = DateTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      );
      
      rtc.adjust(ntpTime);
      Serial.println("\nâœ… RTC synced with NTP (Philippines Time UTC+8)");
      
      char timeStr[12];
      safeTimeFormat(timeStr, ntpTime.hour(), ntpTime.minute(), ntpTime.second());
      Serial.printf("ðŸ“… Current time: %04d/%02d/%02d %s\n", 
                    ntpTime.year(), ntpTime.month(), ntpTime.day(), timeStr);
      return true;
    } else {
      Serial.println("\nâŒ Failed to get NTP time");
      return false;
    }
  }
  return false;
}

// ============= SET MANUAL TIME =============
void setManualTime(int year, int month, int day, int hour, int minute, int second) {
  DateTime manualTime = DateTime(year, month, day, hour, minute, second);
  rtc.adjust(manualTime);
  Serial.println("âœ… Time manually set");
  
  char timeStr[12];
  safeTimeFormat(timeStr, hour, minute, second);
  Serial.printf("ðŸ“… New time: %04d/%02d/%02d %s\n", year, month, day, timeStr);
  
  lcd.clear();
  lcd.print("Time Updated!");
  lcd.setCursor(0, 1);
  lcd.print(timeStr);
  delay(2000);
  
  lcd.clear();
  lcd.print("System Ready!");
  lcd.setCursor(0, 1);
  lcd.print("Place finger...");
}

// ============= CHECK RTC TIME =============
void checkRTC() {
  if (!rtc.begin()) {
    Serial.println("âŒ RTC not found!");
    return;
  }
  
  DateTime now = rtc.now();
  
  Serial.println("\n=== RTC TIME CHECK ===");
  Serial.printf("Raw RTC: %04d-%02d-%02d %02d:%02d:%02d\n", 
                now.year(), now.month(), now.day(),
                now.hour(), now.minute(), now.second());
  
  // Check if time is in 24-hour format (which it should be)
  if (now.hour() >= 0 && now.hour() <= 23) {
    Serial.println("âœ… RTC is in 24-hour format");
    
    // Display in 12-hour format for verification
    int hour12 = now.hour() % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = (now.hour() < 12) ? "AM" : "PM";
    
    Serial.printf("12-hour format: %02d:%02d:%02d %s\n", 
                  hour12, now.minute(), now.second(), ampm);
  } else {
    Serial.println("âŒ RTC has invalid hour value!");
  }
  
  // Calculate expected time for Philippines (UTC+8)
  Serial.println("\nTimezone Info:");
  Serial.println("Philippines is UTC+8");
  Serial.println("Make sure your RTC is set to Philippines time");
  Serial.println("========================\n");
}

// ============= VALIDATE AND FIX RTC =============
bool validateAndFixRTC() {
  if (!rtc.begin()) {
    Serial.println("âŒ RTC not found!");
    return false;
  }
  
  DateTime now = rtc.now();
  
  // Check if RTC has lost power or has invalid time
  if (rtc.lostPower() || now.year() < 2020 || now.year() > 2030) {
    Serial.println("âš ï¸ RTC has invalid date/time! Attempting to sync with NTP...");
    
    // Try to sync with NTP first
    if (WiFi.status() == WL_CONNECTED) {
      if (syncTimeWithNTP()) {
        return true;
      }
    }
    
    // If NTP fails, set to compile time (but this will be in UTC)
    Serial.println("âš ï¸ Setting to compile time (may be off by timezone)...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    
    // Verify again
    now = rtc.now();
    if (now.year() < 2020 || now.year() > 2030) {
      Serial.println("âŒ RTC still returning invalid data after reset!");
      return false;
    }
    
    Serial.println("âœ… RTC reset to compile time");
    return true;
  }
  
  return true;
}

// ============= SAFE TIME FORMATTING (12-HOUR FORMAT) =============
void safeTimeFormat(char* buffer, int hour, int minute, int second) {
  // Clamp values to valid ranges
  if (hour < 0 || hour > 23) hour = 0;
  if (minute < 0 || minute > 59) minute = 0;
  if (second < 0 || second > 59) second = 0;
  
  // Convert to 12-hour format
  int hour12 = hour % 12;
  if (hour12 == 0) hour12 = 12;
  const char* ampm = (hour < 12) ? "AM" : "PM";
  
  sprintf(buffer, "%02d:%02d:%02d %s", hour12, minute, second, ampm);
}

// ============= SAFE DATE FORMATTING =============
void safeDateFormat(char* buffer, int year, int month, int day) {
  // Clamp values to valid ranges
  if (year < 2000 || year > 2100) year = 2024;
  if (month < 1 || month > 12) month = 1;
  if (day < 1 || day > 31) day = 1;
  
  sprintf(buffer, "%04d/%02d/%02d", year, month, day);
}

// ============= SETUP =============
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=== CFSI MONITORING SYSTEM ===");
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Initialize I2C
  Wire.begin(21, 22);
  Wire.setClock(400000);
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");
  lcd.setCursor(0, 1);
  lcd.print("System Starting");
  
  // Load AI assistant settings (mode, WiFi/API/local-server config) early,
  // so we know whether to also join a router WiFi for Cloud AI mode.
  loadAssistantSettings();

  // Self-hosted network: the ESP32 creates its OWN WiFi network instead of
  // joining one, so it needs no router and no internet connection at all.
  // Any phone/laptop can connect directly to this network and reach the
  // dashboard - it works anywhere the device has power, including rooms
  // with no WiFi router present.
  // WIFI_AP_STA keeps this self-hosted AP running at all times; if Cloud AI
  // mode is on, we additionally join the configured router for internet
  // access - the local dashboard/offline features are unaffected either way.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("CFSI_Monitoring", "12345678");
  IPAddress IP = WiFi.softAPIP();
  Serial.print("ðŸ“¡ Self-hosted network started. Connect to \"CFSI_Monitoring\" and open http://");
  Serial.println(IP);

  if (assistantSettings.mode == 1 && assistantSettings.wifiSSID.length() > 0) {
    Serial.println("ðŸŒ Cloud AI enabled - attempting to join internet WiFi in background...");
    WiFi.begin(assistantSettings.wifiSSID.c_str(), assistantSettings.wifiPassword.c_str());
  }
  
  // Initialize fingerprint sensor
  fingerSerial.begin(57600, SERIAL_8N1, 16, 17);
  delay(500);
  
  finger.begin(57600);
  
  lcd.clear();
  lcd.print("Checking Sensor");
  
  if (finger.verifyPassword()) {
    Serial.println("âœ… Fingerprint sensor detected!");
    
    // Get template count
    finger.getTemplateCount();
    Serial.print("ðŸ“Š Templates stored: ");
    Serial.println(finger.templateCount);
  } else {
    Serial.println("âŒ Fingerprint sensor NOT found!");
    Serial.println("âš ï¸ Check wiring: RX=16, TX=17, VCC=3.3V, GND=GND");
    lcd.clear();
    lcd.print("Sensor Error!");
    lcd.setCursor(0, 1);
    lcd.print("Check Wiring!");
    delay(3000);
  }
  
  // Initialize RTC
  Serial.println("\n=== RTC INITIALIZATION ===");
  if (!rtc.begin()) {
    Serial.println("âŒ RTC NOT found! Using software time fallback...");
    Serial.println("âš ï¸ Dates and times will be incorrect until RTC is fixed");
  } else {
    Serial.println("âœ… RTC detected!");
    
    // Validate and fix RTC time
    validateAndFixRTC();
    
    // Show current time
    DateTime now = rtc.now();
    char timeStr[12];
    safeTimeFormat(timeStr, now.hour(), now.minute(), now.second());
    Serial.printf("ðŸ“… Current RTC time: %04d/%02d/%02d %s\n", 
                  now.year(), now.month(), now.day(), timeStr);
  }
  Serial.println("===========================\n");
  
  // After WiFi is connected, try to sync RTC with NTP
  if (WiFi.status() == WL_CONNECTED) {
    syncTimeWithNTP();
  }
  
  // Load time configuration
  loadTimeConfig();
  
  // Migrate old EEPROM data if needed
  migrateEEPROM();
  
  // Load data
  loadStudents();
  loadAttendance();
  buildFingerprintCache();
  rebuildStudentCache();
  
  // Starts the dashboard on the self-hosted network above.
  setupWebServer();
  server.begin();
  
  Serial.print("ðŸ“Š Students loaded: ");
  Serial.println(studentCount);
  Serial.print("ðŸ“Š Attendance records loaded: ");
  Serial.println(attendanceCount);
  
  // Check sensor memory
  checkSensorMemory();
  checkSensorCapacity();
  checkRTC(); // Additional RTC check
  
  // Show "System Ready" on LCD instead of WiFi status
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready!");
  lcd.setCursor(0, 1);
  lcd.print("Place finger...");
  
  lastLCDUpdate = millis();
}

// ============= LOAD TIME CONFIGURATION =============
void loadTimeConfig() {
  int addr = TIME_CONFIG_ADDR;
  
  // Check if configuration exists (first byte is 0xFF if EEPROM is fresh)
  if (EEPROM.read(addr) == 0xFF) {
    // Use default values
    saveTimeConfig();
    Serial.println("ðŸ“… Using default time configuration");
  } else {
    timeConfig.startHour = EEPROM.read(addr);
    addr += sizeof(int);
    timeConfig.startMinute = EEPROM.read(addr);
    addr += sizeof(int);
    timeConfig.endHour = EEPROM.read(addr);
    addr += sizeof(int);
    timeConfig.endMinute = EEPROM.read(addr);
    addr += sizeof(int);
    timeConfig.gracePeriod = EEPROM.read(addr);
    addr += sizeof(int);
    timeConfig.autoDetect = EEPROM.read(addr);
    
    Serial.println("ðŸ“… Loaded time configuration:");
    Serial.printf("   Start: %02d:%02d\n", timeConfig.startHour, timeConfig.startMinute);
    Serial.printf("   End: %02d:%02d\n", timeConfig.endHour, timeConfig.endMinute);
    Serial.printf("   Grace: %d minutes\n", timeConfig.gracePeriod);
    Serial.printf("   Auto-detect: %s\n", timeConfig.autoDetect ? "Yes" : "No");
  }
}

// ============= SAVE TIME CONFIGURATION =============
void saveTimeConfig() {
  int addr = TIME_CONFIG_ADDR;
  
  EEPROM.write(addr, timeConfig.startHour);
  addr += sizeof(int);
  EEPROM.write(addr, timeConfig.startMinute);
  addr += sizeof(int);
  EEPROM.write(addr, timeConfig.endHour);
  addr += sizeof(int);
  EEPROM.write(addr, timeConfig.endMinute);
  addr += sizeof(int);
  EEPROM.write(addr, timeConfig.gracePeriod);
  addr += sizeof(int);
  EEPROM.write(addr, timeConfig.autoDetect);
  
  EEPROM.commit();
  Serial.println("ðŸ’¾ Time configuration saved to EEPROM");
}

// ============= CALCULATE REMARK =============
int calculateRemark(DateTime scanTime, bool isTimeIn) {
  int scanMinutes = scanTime.hour() * 60 + scanTime.minute();
  int startMinutes = timeConfig.startHour * 60 + timeConfig.startMinute;
  int endMinutes = timeConfig.endHour * 60 + timeConfig.endMinute;
  
  if (isTimeIn) {
    // Time In remarks
    if (scanMinutes <= startMinutes + timeConfig.gracePeriod) {
      return 0; // On Time (before or during grace period)
    } else {
      return 1; // Late
    }
  } else {
    // Time Out remarks
    if (scanMinutes >= endMinutes - timeConfig.gracePeriod) {
      return 0; // On Time (after or during grace period before end)
    } else {
      return 2; // Early
    }
  }
}

// ============= CALCULATE DURATION =============
int calculateDuration(const char* timeIn, const char* timeOut) {
  if (strlen(timeIn) == 0 || strlen(timeOut) == 0) {
    return 0;
  }
  
  int inHour, inMin, inSec;
  int outHour, outMin, outSec;
  char inAmpm[3], outAmpm[3];
  
  // Parse time strings with AM/PM
  sscanf(timeIn, "%02d:%02d:%02d %s", &inHour, &inMin, &inSec, inAmpm);
  sscanf(timeOut, "%02d:%02d:%02d %s", &outHour, &outMin, &outSec, outAmpm);
  
  // Convert 12-hour to 24-hour for calculation
  if (strcmp(inAmpm, "PM") == 0 && inHour != 12) inHour += 12;
  if (strcmp(inAmpm, "AM") == 0 && inHour == 12) inHour = 0;
  if (strcmp(outAmpm, "PM") == 0 && outHour != 12) outHour += 12;
  if (strcmp(outAmpm, "AM") == 0 && outHour == 12) outHour = 0;
  
  // Convert to minutes since midnight
  int inMinutes = inHour * 60 + inMin;
  int outMinutes = outHour * 60 + outMin;
  
  // Handle overnight sessions (if time out is earlier than time in, assume next day)
  if (outMinutes < inMinutes) {
    outMinutes += 24 * 60;
  }
  
  int duration = outMinutes - inMinutes;
  
  Serial.printf("Duration calculation: %02d:%02d - %02d:%02d = %d minutes\n", 
                inHour, inMin, outHour, outMin, duration);
  
  return duration;
}

// ============= FORCE RESET EEPROM FUNCTION =============
void forceResetEEPROM() {
  Serial.println("\nâš ï¸ FORCE RESETTING EEPROM TO FACTORY DEFAULTS...");
  
  // Clear all EEPROM
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  
  // Reset all global variables
  studentCount = 0;
  attendanceCount = 0;
  registrationMode = false;
  pendingName = "";
  pendingGrade = "";
  pendingSection = "";
  pendingId = 0;
  registrationStep = 1;
  lastSuccessId = 0;
  
  // Reset time config to defaults
  timeConfig.startHour = 8;
  timeConfig.startMinute = 0;
  timeConfig.endHour = 17;
  timeConfig.endMinute = 0;
  timeConfig.gracePeriod = 15;
  timeConfig.autoDetect = true;
  saveTimeConfig();
  
  // Clear fingerprint sensor
  finger.emptyDatabase();
  
  // Rebuild cache
  buildFingerprintCache();
  rebuildStudentCache();
  
  Serial.println("âœ… EEPROM has been reset to factory defaults");
  Serial.println("ðŸ“Š You can now register new students\n");
  
  lcd.clear();
  lcd.print("EEPROM Reset!");
  lcd.setCursor(0, 1);
  lcd.print("Ready to Register");
  delay(2000);
  
  lcd.clear();
  lcd.print("System Ready!");
  lcd.setCursor(0, 1);
  lcd.print("Place finger...");
}

// ============= REBUILD STUDENT CACHE =============
void rebuildStudentCache() {
  memset(studentIdToIndex, 0, sizeof(studentIdToIndex));
  for (int i = 0; i < studentCount; i++) {
    if (students[i].id <= 200) {
      studentIdToIndex[students[i].id] = i + 1; // +1 because 0 means not found
    }
  }
}

// ============= EEPROM MIGRATION FUNCTION =============
void migrateEEPROM() {
  Serial.println("\n=== CHECKING EEPROM FORMAT ===");
  
  // Read the current student count
  int oldCount = EEPROM.readInt(STUDENT_COUNT_ADDR);
  
  // If count is valid but seems like old format (too high or unreasonable)
  if (oldCount > 0 && oldCount <= MAX_STUDENTS) {
    Serial.print("Found ");
    Serial.print(oldCount);
    Serial.println(" students in old format. Attempting migration...");
    
    // Create temporary array for old data
    struct OldStudent {
      int id;
      char name[20];
      char grade[5];
      char section[2];  // Old section size
      bool isRegistered;
    } oldStudents[MAX_STUDENTS];
    
    // Read old data
    int addr = STUDENT_DATA_START;
    for (int i = 0; i < oldCount; i++) {
      oldStudents[i].id = EEPROM.readInt(addr);
      addr += sizeof(int);
      
      EEPROM.readBytes(addr, oldStudents[i].name, 20);
      oldStudents[i].name[19] = '\0';
      addr += 20;
      
      EEPROM.readBytes(addr, oldStudents[i].grade, 5);
      oldStudents[i].grade[4] = '\0';
      addr += 5;
      
      EEPROM.readBytes(addr, oldStudents[i].section, 2);
      oldStudents[i].section[1] = '\0';
      addr += 2;
      
      oldStudents[i].isRegistered = EEPROM.readBool(addr);
      addr += sizeof(bool);
      
      // Skip old padding (old record size was 32)
      addr += 32 - (sizeof(int) + 20 + 5 + 2 + sizeof(bool));
    }
    
    // Now clear the EEPROM for new format
    for (int i = 0; i < EEPROM_SIZE; i++) {
      EEPROM.write(i, 0);
    }
    
    // Write data in new format
    EEPROM.writeInt(STUDENT_COUNT_ADDR, oldCount);
    addr = STUDENT_DATA_START;
    
    for (int i = 0; i < oldCount; i++) {
      students[i].id = oldStudents[i].id;
      strcpy(students[i].name, oldStudents[i].name);
      strcpy(students[i].grade, oldStudents[i].grade);
      strcpy(students[i].section, oldStudents[i].section); // This will copy the old 1-char section
      students[i].isRegistered = oldStudents[i].isRegistered;
      
      // Write to EEPROM in new format
      EEPROM.writeInt(addr, students[i].id);
      addr += sizeof(int);
      
      EEPROM.writeBytes(addr, students[i].name, 20);
      addr += 20;
      
      EEPROM.writeBytes(addr, students[i].grade, 5);
      addr += 5;
      
      EEPROM.writeBytes(addr, students[i].section, 9);
      addr += 9;
      
      EEPROM.writeBool(addr, students[i].isRegistered);
      addr += sizeof(bool);
      
      // Add padding to reach STUDENT_RECORD_SIZE
      int padding = STUDENT_RECORD_SIZE - (sizeof(int) + 20 + 5 + 9 + sizeof(bool));
      for (int j = 0; j < padding; j++) {
        EEPROM.write(addr + j, 0);
      }
      addr += padding;
    }
    
    EEPROM.commit();
    Serial.println("âœ… Migration complete!");
  } else {
    Serial.println("No migration needed or EEPROM already in new format.");
  }
  Serial.println("===============================\n");
}

// ============= WEBSERVER ROUTES =============
void setupWebServer() {
  server.on("/", []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });
  
  server.on("/api/locations", []() {
    String json = "[";
    for (int i = 0; i < LOCATION_COUNT; i++) {
      if (i > 0) json += ",";
      json += "\"" + String(locationNames[i]) + "\"";
    }
    json += "]";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/stats", []() {
    DateTime now;
    if (!validateAndFixRTC()) {
      now = DateTime(F(__DATE__), F(__TIME__));
    } else {
      now = rtc.now();
    }
    
    int todayCount = 0;
    int lateToday = 0;
    int activeNow = 0;
    char today[11];
    safeDateFormat(today, now.year(), now.month(), now.day());
    
    for (int i = 0; i < attendanceCount; i++) {
      if (strcmp(attendanceRecords[i].date, today) == 0) {
        todayCount++;
        if (attendanceRecords[i].remark == 1) lateToday++; // Late
        if (attendanceRecords[i].isActive) activeNow++;
      }
    }
    
    String json = "{\"totalStudents\":" + String(studentCount) + 
                  ",\"todayAttendance\":" + String(todayCount) + 
                  ",\"lateToday\":" + String(lateToday) + 
                  ",\"activeNow\":" + String(activeNow) + "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/assistant", HTTP_POST, []() {
    String query = server.hasArg("query") ? server.arg("query") : "";
    String answer = "";
    String source = "offline";

    if (assistantSettings.mode == 1) {
      answer = getCloudAIAnswer(query);
      if (answer.length() > 0) {
        source = "cloud";
      } else {
        answer = "âš ï¸ Couldn't reach the cloud AI (check internet/API key). Falling back to offline mode:\n\n" + processAssistantQuery(query);
      }
    } else if (assistantSettings.mode == 2) {
      answer = getLocalAIAnswer(query);
      if (answer.length() > 0) {
        source = "local";
      } else {
        answer = "âš ï¸ Couldn't reach the local AI server (check it's running and reachable). Falling back to offline mode:\n\n" + processAssistantQuery(query);
      }
    } else {
      answer = processAssistantQuery(query);
    }

    String json = "{\"answer\":\"" + jsonEscape(answer) + "\",\"source\":\"" + source + "\"}";
    server.send(200, "application/json", json);
  });

  server.on("/api/assistant-config", []() {
    String json = "{";
    json += "\"mode\":" + String(assistantSettings.mode) + ",";
    json += "\"wifiSSID\":\"" + jsonEscape(assistantSettings.wifiSSID) + "\",";
    json += "\"hasWifiPassword\":" + String(assistantSettings.wifiPassword.length() > 0 ? "true" : "false") + ",";
    json += "\"hasApiKey\":" + String(assistantSettings.apiKey.length() > 0 ? "true" : "false") + ",";
    json += "\"apiEndpoint\":\"" + jsonEscape(assistantSettings.apiEndpoint) + "\",";
    json += "\"apiModel\":\"" + jsonEscape(assistantSettings.apiModel) + "\",";
    json += "\"localURL\":\"" + jsonEscape(assistantSettings.localURL) + "\",";
    json += "\"localModel\":\"" + jsonEscape(assistantSettings.localModel) + "\",";
    json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/save-assistant-config", HTTP_POST, []() {
    if (server.hasArg("mode")) assistantSettings.mode = server.arg("mode").toInt();
    if (server.hasArg("wifiSSID")) assistantSettings.wifiSSID = server.arg("wifiSSID");
    if (server.hasArg("wifiPassword") && server.arg("wifiPassword").length() > 0) assistantSettings.wifiPassword = server.arg("wifiPassword");
    if (server.hasArg("apiKey") && server.arg("apiKey").length() > 0) assistantSettings.apiKey = server.arg("apiKey");
    if (server.hasArg("apiEndpoint") && server.arg("apiEndpoint").length() > 0) assistantSettings.apiEndpoint = server.arg("apiEndpoint");
    if (server.hasArg("apiModel") && server.arg("apiModel").length() > 0) assistantSettings.apiModel = server.arg("apiModel");
    if (server.hasArg("localURL")) assistantSettings.localURL = server.arg("localURL");
    if (server.hasArg("localModel") && server.arg("localModel").length() > 0) assistantSettings.localModel = server.arg("localModel");

    saveAssistantSettings();

    if (assistantSettings.mode == 1) {
      ensureInternetConnected(); // best-effort, doesn't block the response either way
    }

    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/time-config", []() {
    String json = "{";
    json += "\"startHour\":" + String(timeConfig.startHour) + ",";
    json += "\"startMinute\":" + String(timeConfig.startMinute) + ",";
    json += "\"endHour\":" + String(timeConfig.endHour) + ",";
    json += "\"endMinute\":" + String(timeConfig.endMinute) + ",";
    json += "\"gracePeriod\":" + String(timeConfig.gracePeriod) + ",";
    json += "\"autoDetect\":" + String(timeConfig.autoDetect ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/save-time-config", HTTP_POST, []() {
    if (server.hasArg("startHour") && server.hasArg("startMinute") && 
        server.hasArg("endHour") && server.hasArg("endMinute") && 
        server.hasArg("gracePeriod") && server.hasArg("autoDetect")) {
      
      timeConfig.startHour = server.arg("startHour").toInt();
      timeConfig.startMinute = server.arg("startMinute").toInt();
      timeConfig.endHour = server.arg("endHour").toInt();
      timeConfig.endMinute = server.arg("endMinute").toInt();
      timeConfig.gracePeriod = server.arg("gracePeriod").toInt();
      timeConfig.autoDetect = server.arg("autoDetect") == "true";
      
      saveTimeConfig();
      
      server.send(200, "application/json", "{\"success\":true}");
    } else {
      server.send(400, "application/json", "{\"success\":false}");
    }
  });
  
  server.on("/api/check-rtc", []() {
    DateTime now;
    bool valid = validateAndFixRTC();
    if (valid) {
      now = rtc.now();
    } else {
      now = DateTime(F(__DATE__), F(__TIME__));
    }
    
    char dateStr[11];
    char timeStr[12];
    safeDateFormat(dateStr, now.year(), now.month(), now.day());
    safeTimeFormat(timeStr, now.hour(), now.minute(), now.second());
    
    String json = "{\"date\":\"" + String(dateStr) + 
                  "\",\"time\":\"" + String(timeStr) + 
                  "\",\"valid\":" + String(valid ? "true" : "false") + "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/sync-time", []() {
    String response;
    if (WiFi.status() == WL_CONNECTED) {
      if (syncTimeWithNTP()) {
        DateTime now = rtc.now();
        char timeStr[12];
        char dateStr[11];
        safeTimeFormat(timeStr, now.hour(), now.minute(), now.second());
        safeDateFormat(dateStr, now.year(), now.month(), now.day());
        
        response = "{\"success\":true,\"message\":\"Time synced successfully\",\"date\":\"" + 
                   String(dateStr) + "\",\"time\":\"" + String(timeStr) + "\"}";
      } else {
        response = "{\"success\":false,\"message\":\"Failed to sync time\"}";
      }
    } else {
      response = "{\"success\":false,\"message\":\"WiFi not connected\"}";
    }
    server.send(200, "application/json", response);
  });
  
  server.on("/api/set-time", HTTP_POST, []() {
    if (server.hasArg("year") && server.hasArg("month") && server.hasArg("day") &&
        server.hasArg("hour") && server.hasArg("minute") && server.hasArg("second")) {
      
      int year = server.arg("year").toInt();
      int month = server.arg("month").toInt();
      int day = server.arg("day").toInt();
      int hour = server.arg("hour").toInt();
      int minute = server.arg("minute").toInt();
      int second = server.arg("second").toInt();
      
      setManualTime(year, month, day, hour, minute, second);
      
      server.send(200, "application/json", "{\"success\":true}");
    } else {
      server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing parameters\"}");
    }
  });
  
  server.on("/api/current-time", []() {
    DateTime now;
    if (!validateAndFixRTC()) {
      now = DateTime(F(__DATE__), F(__TIME__));
    } else {
      now = rtc.now();
    }
    
    char dateStr[11];
    char timeStr[12];
    safeDateFormat(dateStr, now.year(), now.month(), now.day());
    safeTimeFormat(timeStr, now.hour(), now.minute(), now.second());
    
    String json = "{\"date\":\"" + String(dateStr) + 
                  "\",\"time\":\"" + String(timeStr) + 
                  "\",\"hour24\":" + String(now.hour()) + 
                  ",\"minute\":" + String(now.minute()) + 
                  ",\"second\":" + String(now.second()) + "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/sync-ntp", []() {
    String response;
    if (WiFi.status() == WL_CONNECTED) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        DateTime ntpTime = DateTime(
          timeinfo.tm_year + 1900,
          timeinfo.tm_mon + 1,
          timeinfo.tm_mday,
          timeinfo.tm_hour,
          timeinfo.tm_min,
          timeinfo.tm_sec
        );
        
        rtc.adjust(ntpTime);
        
        char timeStr[12];
        safeTimeFormat(timeStr, ntpTime.hour(), ntpTime.minute(), ntpTime.second());
        
        response = "{\"success\":true,\"message\":\"RTC synced with NTP\",\"time\":\"" + String(timeStr) + "\"}";
      } else {
        response = "{\"success\":false,\"message\":\"Failed to get NTP time\"}";
      }
    } else {
      response = "{\"success\":false,\"message\":\"WiFi not connected\"}";
    }
    server.send(200, "application/json", response);
  });
  
  server.on("/api/clear-active-sessions", []() {
    int cleared = 0;
    for (int i = 0; i < attendanceCount; i++) {
      if (attendanceRecords[i].isActive) {
        attendanceRecords[i].isActive = false;
        cleared++;
      }
    }
    if (cleared > 0) {
      saveAttendance();
    }
    String json = "{\"success\":true,\"cleared\":" + String(cleared) + "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/students", []() {
    String json = "{\"students\":[";
    for (int i = 0; i < studentCount; i++) {
      if (i > 0) json += ",";
      json += "{\"id\":" + String(students[i].id) + 
              ",\"name\":\"" + String(students[i].name) + 
              "\",\"grade\":\"" + String(students[i].grade) + 
              "\",\"section\":\"" + String(students[i].section) + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/attendance", []() {
    String json = "{\"records\":[";
    int startIdx = max(0, attendanceCount-200);
    for (int i = startIdx; i < attendanceCount; i++) {
      Student* s = findStudentById(attendanceRecords[i].studentId);
      if (s) {
        if (i > startIdx) json += ",";
        json += "{\"name\":\"" + String(s->name) + 
                "\",\"grade\":\"" + String(s->grade) + 
                "\",\"section\":\"" + String(s->section) + 
                "\",\"location\":\"" + getLocationName(attendanceRecords[i].location) +
                "\",\"date\":\"" + String(attendanceRecords[i].date) + 
                "\",\"timeIn\":\"" + String(attendanceRecords[i].timeIn) + 
                "\",\"timeOut\":\"" + String(attendanceRecords[i].timeOut) + 
                "\",\"duration\":" + String(attendanceRecords[i].duration) +
                ",\"remark\":\"" + String(getRemarkText(attendanceRecords[i].remark)) +
                "\",\"isActive\":" + String(attendanceRecords[i].isActive ? "true" : "false") + "}";
      }
    }
    json += "]}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/export-attendance", []() {
    String json = "{\"records\":[";
    for (int i = 0; i < attendanceCount; i++) {
      Student* s = findStudentById(attendanceRecords[i].studentId);
      if (s) {
        if (i > 0) json += ",";
        json += "{\"name\":\"" + String(s->name) + 
                "\",\"grade\":\"" + String(s->grade) + 
                "\",\"section\":\"" + String(s->section) + 
                "\",\"location\":\"" + getLocationName(attendanceRecords[i].location) +
                "\",\"date\":\"" + String(attendanceRecords[i].date) + 
                "\",\"timeIn\":\"" + String(attendanceRecords[i].timeIn) + 
                "\",\"timeOut\":\"" + String(attendanceRecords[i].timeOut) + 
                "\",\"duration\":" + String(attendanceRecords[i].duration) +
                ",\"remark\":\"" + String(getRemarkText(attendanceRecords[i].remark)) +
                "\",\"isActive\":" + String(attendanceRecords[i].isActive ? "true" : "false") + "}";
      }
    }
    json += "]}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/attendance-summary", []() {
    DateTime now;
    if (!validateAndFixRTC()) {
      now = DateTime(F(__DATE__), F(__TIME__));
    } else {
      now = rtc.now();
    }
    
    char today[11];
    safeDateFormat(today, now.year(), now.month(), now.day());
    
    int todayCount = 0;
    int activeNow = 0;
    int lateToday = 0;
    int ontimeToday = 0;
    int locationCounts[LOCATION_COUNT] = {0};
    int locationLate[LOCATION_COUNT] = {0};
    int locationOntime[LOCATION_COUNT] = {0};
    
    for (int i = 0; i < attendanceCount; i++) {
      if (strcmp(attendanceRecords[i].date, today) == 0) {
        todayCount++;
        if (attendanceRecords[i].location >= 0 && attendanceRecords[i].location < LOCATION_COUNT) {
          locationCounts[attendanceRecords[i].location]++;
          if (attendanceRecords[i].isActive) activeNow++;
          
          if (attendanceRecords[i].remark == 1) { // Late
            lateToday++;
            locationLate[attendanceRecords[i].location]++;
          } else if (attendanceRecords[i].remark == 0) { // On Time
            ontimeToday++;
            locationOntime[attendanceRecords[i].location]++;
          }
        }
      }
    }
    
    String json = "{\"totalToday\":" + String(todayCount) + 
                  ",\"activeNow\":" + String(activeNow) + 
                  ",\"lateToday\":" + String(lateToday) + 
                  ",\"ontimeToday\":" + String(ontimeToday) + 
                  ",\"locationSummary\":{";
    
    for (int i = 0; i < LOCATION_COUNT; i++) {
      if (i > 0) json += ",";
      json += "\"" + String(locationNames[i]) + "\":" + String(locationCounts[i]);
    }
    json += "},\"locationLate\":{";
    for (int i = 0; i < LOCATION_COUNT; i++) {
      if (i > 0) json += ",";
      json += "\"" + String(locationNames[i]) + "\":" + String(locationLate[i]);
    }
    json += "},\"locationOntime\":{";
    for (int i = 0; i < LOCATION_COUNT; i++) {
      if (i > 0) json += ",";
      json += "\"" + String(locationNames[i]) + "\":" + String(locationOntime[i]);
    }
    json += "}}";
    
    server.send(200, "application/json", json);
  });
  
  // Filtered attendance endpoint
  server.on("/api/filtered-attendance", []() {
    DateTime now;
    if (!validateAndFixRTC()) {
      now = DateTime(F(__DATE__), F(__TIME__));
    } else {
      now = rtc.now();
    }
    
    char today[11];
    safeDateFormat(today, now.year(), now.month(), now.day());
    
    String filter = server.hasArg("filter") ? server.arg("filter") : "all";
    String location = server.hasArg("location") ? server.arg("location") : "";
    
    String json = "{\"records\":[";
    bool first = true;
    
    for (int i = 0; i < attendanceCount; i++) {
      bool include = false;
      
      if (filter == "all") {
        include = true;
      } else if (filter == "today") {
        include = (strcmp(attendanceRecords[i].date, today) == 0);
      } else if (filter == "active") {
        include = attendanceRecords[i].isActive;
      } else if (filter == "late") {
        include = (strcmp(attendanceRecords[i].date, today) == 0 && attendanceRecords[i].remark == 1);
        // Filter by location if specified
        if (include && location != "") {
          include = (String(locationNames[attendanceRecords[i].location]) == location);
        }
      } else if (filter == "ontime") {
        include = (strcmp(attendanceRecords[i].date, today) == 0 && attendanceRecords[i].remark == 0);
        // Filter by location if specified
        if (include && location != "") {
          include = (String(locationNames[attendanceRecords[i].location]) == location);
        }
      } else if (filter == "location") {
        include = (strcmp(attendanceRecords[i].date, today) == 0 && 
                  String(locationNames[attendanceRecords[i].location]) == location);
      }
      
      if (include) {
        Student* s = findStudentById(attendanceRecords[i].studentId);
        if (s) {
          if (!first) json += ",";
          first = false;
          
          json += "{\"name\":\"" + String(s->name) + 
                  "\",\"grade\":\"" + String(s->grade) + 
                  "\",\"section\":\"" + String(s->section) + 
                  "\",\"location\":\"" + getLocationName(attendanceRecords[i].location) +
                  "\",\"timeIn\":\"" + String(attendanceRecords[i].timeIn) + 
                  "\",\"timeOut\":\"" + String(attendanceRecords[i].timeOut) + 
                  "\",\"remark\":\"" + String(getRemarkText(attendanceRecords[i].remark)) +
                  "\",\"isActive\":" + String(attendanceRecords[i].isActive ? "true" : "false") + "}";
        }
      }
    }
    
    json += "]}";
    server.send(200, "application/json", json);
  });
  
  // Sensor capacity endpoint
  server.on("/api/sensor-capacity", []() {
    finger.getTemplateCount();
    String json = "{\"current\":" + String(finger.templateCount) + 
                  ",\"max\":" + String(MAX_FINGERPRINT_CAPACITY) + "}";
    server.send(200, "application/json", json);
  });
  
  // Reset EEPROM endpoint
  server.on("/api/reset-eeprom", []() {
    forceResetEEPROM();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"EEPROM reset to factory defaults\"}");
  });
  
  // ============= REGISTER-START HANDLER =============
  server.on("/api/register-start", HTTP_POST, []() {
    if (server.hasArg("name") && server.hasArg("grade") && server.hasArg("section")) {
      
      // Check if already in registration mode
      if (registrationMode) {
        server.send(200, "application/json", "{\"success\":false, \"message\":\"Registration already in progress\"}");
        return;
      }
      
      // Make sure sensor is ready
      if (!finger.verifyPassword()) {
        server.send(200, "application/json", "{\"success\":false, \"message\":\"Fingerprint sensor not responding\"}");
        return;
      }
      
      // Check sensor capacity
      finger.getTemplateCount();
      if (finger.templateCount >= MAX_FINGERPRINT_CAPACITY - 5) {
        server.send(200, "application/json", "{\"success\":false, \"message\":\"Fingerprint sensor is almost full. Please delete some fingerprints.\"}");
        return;
      }
      
      pendingName = server.arg("name");
      pendingGrade = server.arg("grade");
      pendingSection = server.arg("section");
      
      if (studentCount >= MAX_STUDENTS) {
        server.send(200, "application/json", "{\"success\":false, \"message\":\"Maximum students reached\"}");
        return;
      }
      
      pendingId = getNextAvailableId();
      
      // Clear any pending data in sensor
      finger.getImage();
      delay(100);
      finger.getImage(); // Double clear
      
      // Clear lastSuccessId when starting a new registration
      lastSuccessId = 0;
      
      // Reset all registration flags
      registrationMode = true;
      registrationStep = 1;  // Start at step 1
      registrationStartTime = millis();
      
      lcd.clear();
      lcd.print("Register: ");
      lcd.setCursor(0, 1);
      lcd.print(pendingName.substring(0, 16));
      delay(2000);
      
      lcd.clear();
      lcd.print("Place finger");
      lcd.setCursor(0, 1);
      lcd.print("to register...");
      
      Serial.println("ðŸ“ Registration started for: " + pendingName);
      Serial.print("ðŸ†” ID: ");
      Serial.println(pendingId);
      Serial.print("ðŸ“Š Sensor capacity: ");
      Serial.print(finger.templateCount);
      Serial.print("/");
      Serial.println(MAX_FINGERPRINT_CAPACITY);
      
      server.send(200, "application/json", "{\"success\":true}");
    } else {
      server.send(400, "application/json", "{\"success\":false}");
    }
  });
  
  // ============= REGISTER-STATUS HANDLER =============
  server.on("/api/register-status", []() {
    String json;
    
    if (registrationMode) {
      // Still in registration mode
      if (registrationStep == 1) {
        json = "{\"status\":\"waiting\"}";
      } else if (registrationStep == 2) {
        json = "{\"status\":\"scan1\"}";
      } else if (registrationStep == 3) {
        json = "{\"status\":\"scan2\"}";
      } else if (registrationStep == 4) {
        json = "{\"status\":\"saving\"}";
      } else {
        json = "{\"status\":\"waiting\"}";
      }
    } else {
      // Registration mode is false - check if we have a successful registration
      if (lastSuccessId > 0) {
        // Find the student that was just registered
        bool found = false;
        for (int i = 0; i < studentCount; i++) {
          if (students[i].id == lastSuccessId) {
            json = "{\"status\":\"success\",\"name\":\"" + String(students[i].name) + 
                   "\",\"grade\":\"" + String(students[i].grade) + 
                   "\",\"section\":\"" + String(students[i].section) + 
                   "\",\"id\":" + String(students[i].id) + "}";
            found = true;
            Serial.print("âœ… Sending success for ID: ");
            Serial.println(lastSuccessId);
            break;
          }
        }
        if (!found) {
          json = "{\"status\":\"failed\",\"message\":\"Student not found in database\"}";
        }
      } else {
        json = "{\"status\":\"failed\",\"message\":\"Registration cancelled\"}";
      }
    }
    
    server.send(200, "application/json", json);
  });
  
  server.on("/api/register-cancel", []() {
    resetRegistration();
    lcd.clear();
    showMainMenu();
    server.send(200, "application/json", "{\"success\":true}");
    Serial.println("âŒ Registration cancelled");
  });
  
  server.on("/api/start-scan", []() {
    if (server.hasArg("location")) {
      selectedLocation = server.arg("location").toInt();
      if (selectedLocation < 0 || selectedLocation >= LOCATION_COUNT) selectedLocation = 0;
    }
    
    fingerprintMode = true;
    lcd.clear();
    lcd.print(locationNames[selectedLocation]);
    lcd.setCursor(0, 1);
    lcd.print("Ready...");
    server.send(200, "application/json", "{\"success\":true}");
    Serial.print("ðŸ‘† Scanning at: ");
    Serial.println(locationNames[selectedLocation]);
  });
  
  server.on("/api/scan-result", []() {
    if (currentScanResult != "") {
      server.send(200, "application/json", currentScanResult);
      currentScanResult = "";
    } else {
      server.send(200, "application/json", "{\"scanned\":false}");
    }
  });
  
  server.on("/api/cancel-scan", []() {
    fingerprintMode = false;
    lcd.clear();
    showMainMenu();
    server.send(200, "application/json", "{\"success\":true}");
  });
  
  server.on("/api/delete-student", HTTP_POST, []() {
    if (server.hasArg("id")) {
      int id = server.arg("id").toInt();
      
      for (int i = 0; i < studentCount; i++) {
        if (students[i].id == id) {
          // Delete from fingerprint sensor
          Serial.print("ðŸ—‘ï¸ Deleting fingerprint ID ");
          Serial.print(id);
          Serial.print(" from sensor... ");
          
          uint8_t p = finger.deleteModel(id);
          if (p == FINGERPRINT_OK) {
            Serial.println("âœ… Success");
          } else {
            Serial.print("âŒ Failed: ");
            Serial.println(getErrorMessage(p));
          }
          
          // Remove from array
          for (int j = i; j < studentCount-1; j++) students[j] = students[j+1];
          studentCount--;
          
          // Save to EEPROM
          saveStudents();
          
          // Rebuild cache
          buildFingerprintCache();
          rebuildStudentCache();
          
          Serial.print("ðŸ—‘ï¸ Deleted student ID: ");
          Serial.println(id);
          
          server.send(200, "application/json", "{\"success\":true}");
          return;
        }
      }
      
      server.send(404, "application/json", "{\"success\":false,\"message\":\"Student not found\"}");
    } else {
      server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing ID\"}");
    }
  });
  
  // Debug endpoints
  server.on("/api/test-sensor", []() {
    String result;
    if (finger.verifyPassword()) {
      finger.getTemplateCount();
      
      // Test image capture
      uint8_t p = finger.getImage();
      int confidence = 0;
      if (p == FINGERPRINT_OK) {
        confidence = 100;
      }
      
      result = "{\"success\":true,\"message\":\"Sensor OK\",\"templates\":" + String(finger.templateCount) + ",\"confidence\":" + String(confidence) + "}";
    } else {
      result = "{\"success\":false,\"message\":\"Sensor not responding\"}";
    }
    server.send(200, "application/json", result);
  });
  
  server.on("/api/debug-state", []() {
    String json = "{";
    json += "\"registrationMode\":" + String(registrationMode ? "true" : "false") + ",";
    json += "\"registrationStep\":" + String(registrationStep) + ",";
    json += "\"pendingId\":" + String(pendingId) + ",";
    json += "\"lastSuccessId\":" + String(lastSuccessId) + ",";
    json += "\"studentCount\":" + String(studentCount) + ",";
    json += "\"attendanceCount\":" + String(attendanceCount) + "";
    json += "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/clear-all-data", []() {
    String response;
    
    // Clear EEPROM
    for (int i = 0; i < EEPROM_SIZE; i++) {
      EEPROM.write(i, 0);
    }
    EEPROM.commit();
    
    // Reset time config to defaults
    timeConfig.startHour = 8;
    timeConfig.startMinute = 0;
    timeConfig.endHour = 17;
    timeConfig.endMinute = 0;
    timeConfig.gracePeriod = 15;
    timeConfig.autoDetect = true;
    saveTimeConfig();
    
    // Clear all fingerprints from sensor
    Serial.println("ðŸ—‘ï¸ Deleting all fingerprints from sensor...");
    uint8_t p = finger.emptyDatabase();
    
    if (p == FINGERPRINT_OK) {
      Serial.println("âœ… All fingerprints deleted from sensor");
      response = "{\"success\":true,\"message\":\"EEPROM and fingerprints cleared\"}";
    } else {
      Serial.print("âŒ Failed to delete fingerprints. Error: ");
      Serial.println(getErrorMessage(p));
      response = "{\"success\":true,\"message\":\"EEPROM cleared but sensor error: " + getErrorMessage(p) + "\"}";
    }
    
    // Reset local data
    studentCount = 0;
    attendanceCount = 0;
    resetRegistration();
    lastSuccessId = 0;
    
    // Rebuild cache
    buildFingerprintCache();
    rebuildStudentCache();
    
    server.send(200, "application/json", response);
    Serial.println("âš ï¸ All data cleared");
  });
  
  server.on("/api/clear-fingerprints", []() {
    Serial.println("ðŸ—‘ï¸ Deleting all fingerprints from sensor...");
    uint8_t p = finger.emptyDatabase();
    
    String response;
    if (p == FINGERPRINT_OK) {
      response = "{\"success\":true,\"message\":\"All fingerprints deleted\"}";
      Serial.println("âœ… All fingerprints deleted from sensor");
    } else {
      response = "{\"success\":false,\"message\":\"" + getErrorMessage(p) + "\"}";
      Serial.print("âŒ Failed to delete fingerprints. Error: ");
      Serial.println(getErrorMessage(p));
    }
    
    // Also remove students from database if they have no fingerprints
    for (int i = studentCount-1; i >= 0; i--) {
      if (finger.loadModel(students[i].id) != FINGERPRINT_OK) {
        for (int j = i; j < studentCount-1; j++) students[j] = students[j+1];
        studentCount--;
      }
    }
    saveStudents();
    buildFingerprintCache();
    rebuildStudentCache();
    
    server.send(200, "application/json", response);
  });
  
  server.on("/api/list-fingerprints", []() {
    String json = "{\"ids\":[";
    String inDB = ",\"inDatabase\":[";
    bool firstId = true;
    bool firstDB = true;
    
    finger.getTemplateCount();
    int found = 0;
    
    for (int id = 1; id <= 200 && found < finger.templateCount; id++) {
      if (finger.loadModel(id) == FINGERPRINT_OK) {
        if (!firstId) json += ",";
        json += String(id);
        firstId = false;
        found++;
        
        // Check if in database
        bool inDatabase = false;
        for (int i = 0; i < studentCount; i++) {
          if (students[i].id == id) {
            inDatabase = true;
            break;
          }
        }
        
        if (!firstDB) inDB += ",";
        inDB += inDatabase ? "true" : "false";
        firstDB = false;
      }
    }
    
    json += "]" + inDB + "]}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/delete-fingerprint", HTTP_POST, []() {
    if (server.hasArg("id")) {
      int id = server.arg("id").toInt();
      
      Serial.print("ðŸ—‘ï¸ Deleting fingerprint ID ");
      Serial.print(id);
      Serial.print(" from sensor... ");
      
      uint8_t p = finger.deleteModel(id);
      
      String response;
      if (p == FINGERPRINT_OK) {
        response = "{\"success\":true,\"message\":\"Fingerprint " + String(id) + " deleted\"}";
        Serial.println("âœ… Success");
        
        // Also remove from student database if exists
        for (int i = 0; i < studentCount; i++) {
          if (students[i].id == id) {
            for (int j = i; j < studentCount-1; j++) students[j] = students[j+1];
            studentCount--;
            saveStudents();
            break;
          }
        }
        
        buildFingerprintCache();
        rebuildStudentCache();
        
      } else {
        response = "{\"success\":false,\"message\":\"" + getErrorMessage(p) + "\"}";
        Serial.print("âŒ Failed: ");
        Serial.println(getErrorMessage(p));
      }
      
      server.send(200, "application/json", response);
    } else {
      server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing ID\"}");
    }
  });
  
  server.on("/api/check-consistency", []() {
    int studentsInDB = studentCount;
    int fingerprintsInSensor = 0;
    int matching = 0;
    int orphaned = 0;
    int missing = 0;
    
    finger.getTemplateCount();
    int sensorCount = finger.templateCount;
    
    // Count fingerprints in sensor
    for (int id = 1; id <= 200 && fingerprintsInSensor < sensorCount; id++) {
      if (finger.loadModel(id) == FINGERPRINT_OK) {
        fingerprintsInSensor++;
        
        // Check if in database
        bool found = false;
        for (int i = 0; i < studentCount; i++) {
          if (students[i].id == id) {
            found = true;
            matching++;
            break;
          }
        }
        if (!found) orphaned++;
      }
    }
    
    // Check for missing in sensor
    for (int i = 0; i < studentCount; i++) {
      if (finger.loadModel(students[i].id) != FINGERPRINT_OK) {
        missing++;
      }
    }
    
    String json = "{\"studentsInDB\":" + String(studentsInDB) + 
                  ",\"fingerprintsInSensor\":" + String(fingerprintsInSensor) + 
                  ",\"matching\":" + String(matching) + 
                  ",\"orphaned\":" + String(orphaned) + 
                  ",\"missing\":" + String(missing) + "}";
    
    server.send(200, "application/json", json);
  });
  
  server.on("/api/clean-orphaned", []() {
    int cleaned = 0;
    
    finger.getTemplateCount();
    int sensorCount = finger.templateCount;
    
    for (int id = 1; id <= 200 && cleaned < sensorCount; id++) {
      if (finger.loadModel(id) == FINGERPRINT_OK) {
        bool inDatabase = false;
        for (int i = 0; i < studentCount; i++) {
          if (students[i].id == id) {
            inDatabase = true;
            break;
          }
        }
        if (!inDatabase) {
          finger.deleteModel(id);
          cleaned++;
        }
      }
    }
    
    String json = "{\"success\":true,\"cleaned\":" + String(cleaned) + "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/registration-state", []() {
    String json = "{\"registrationMode\":" + String(registrationMode ? "true" : "false") + 
                  ",\"registrationStep\":" + String(registrationStep) + 
                  ",\"pendingId\":" + String(pendingId) + 
                  ",\"lastSuccessId\":" + String(lastSuccessId) + "}";
    server.send(200, "application/json", json);
  });
}

// ============= MAIN LOOP =============
void loop() {
  server.handleClient();
  
  if (fingerprintMode) {
    quickScan();
  }
  
  if (registrationMode) {
    handleRegistration();
  }
  
  if (millis() - lastLCDUpdate > LCD_UPDATE_INTERVAL && !fingerprintMode && !registrationMode) {
    lastLCDUpdate = millis();
    showMainMenu();
  }
  
  delay(WEBSERVER_PROCESS_INTERVAL);
}

// ============= OPTIMIZED FAST SCAN FUNCTION =============
void quickScan() {
  if (millis() - lastScanCheck < SCAN_INTERVAL_MS) {
    return;
  }
  lastScanCheck = millis();
  
  uint8_t p = finger.getImage();
  
  if (p == FINGERPRINT_OK) {
    p = finger.image2Tz();
    if (p == FINGERPRINT_OK) {
      p = finger.fingerFastSearch();
      
      if (p == FINGERPRINT_OK) {
        int fingerprintId = finger.fingerID;
        
        // Use cache for faster student lookup
        int studentIndex = -1;
        if (fingerprintId <= 200) {
          studentIndex = studentIdToIndex[fingerprintId] - 1;
        }
        
        // Fallback to linear search if cache miss
        if (studentIndex < 0) {
          for (int i = 0; i < studentCount; i++) {
            if (students[i].id == fingerprintId) {
              studentIndex = i;
              break;
            }
          }
        }
        
        if (studentIndex >= 0) {
          Student* student = &students[studentIndex];
          
          // Get current time with validation
          DateTime now;
          if (!validateAndFixRTC()) {
            now = DateTime(F(__DATE__), F(__TIME__));
            Serial.println("âš ï¸ Using compile time as fallback");
          } else {
            now = rtc.now();
          }
          
          // Format current date
          char today[11];
          safeDateFormat(today, now.year(), now.month(), now.day());
          
          // Format current time in 12-hour format
          char timeStr[12];
          safeTimeFormat(timeStr, now.hour(), now.minute(), now.second());
          
          // Debug print
          Serial.printf("Current time: %04d/%02d/%02d %s\n", 
                        now.year(), now.month(), now.day(), timeStr);
          
          // Check if student has an active session (checked in but not checked out)
          int activeIndex = -1;
          for (int i = attendanceCount - 1; i >= 0; i--) {
            if (attendanceRecords[i].studentId == fingerprintId && 
                strcmp(attendanceRecords[i].date, today) == 0 &&
                attendanceRecords[i].isActive) {
              activeIndex = i;
              break;
            }
          }
          
          if (activeIndex >= 0) {
            // Found active session - this is TIME OUT
            Serial.printf("Found active session at index %d - recording TIME OUT\n", activeIndex);
            
            // Record time out
            strcpy(attendanceRecords[activeIndex].timeOut, timeStr);
            attendanceRecords[activeIndex].isActive = false;
            
            // Calculate duration
            attendanceRecords[activeIndex].duration = calculateDuration(
              attendanceRecords[activeIndex].timeIn, 
              attendanceRecords[activeIndex].timeOut
            );
            
            // Calculate remark for time out
            attendanceRecords[activeIndex].remark = calculateRemark(now, false);
            
            // Auto-save immediately
            saveAttendance();
            
            String remarkText = getRemarkText(attendanceRecords[activeIndex].remark);
            currentScanResult = "{\"scanned\":true,\"name\":\"" + String(student->name) + 
                               "\",\"grade\":\"" + String(student->grade) + 
                               "\",\"section\":\"" + String(student->section) + 
                               "\",\"location\":\"" + getLocationName(attendanceRecords[activeIndex].location) +
                               "\",\"time\":\"" + String(timeStr) + 
                               "\",\"type\":\"Time Out\"" +
                               ",\"remark\":\"" + remarkText + "\"}";
            
            lcd.clear();
            lcd.print(student->name);
            lcd.setCursor(0, 1);
            lcd.print("OUT ");
            lcd.print(timeStr);
            lcd.print(" ");
            lcd.print(remarkText.substring(0, 4));
            
            Serial.printf("âœ… Time Out recorded for %s at %s\n", student->name, timeStr);
            
          } else {
            // No active session - this is TIME IN
            Serial.println("No active session found - recording TIME IN");
            
            if (attendanceCount < MAX_ATTENDANCE) {
              int remark = calculateRemark(now, true);
              
              attendanceRecords[attendanceCount].studentId = fingerprintId;
              strcpy(attendanceRecords[attendanceCount].timeIn, timeStr);
              strcpy(attendanceRecords[attendanceCount].timeOut, "");
              strcpy(attendanceRecords[attendanceCount].date, today);
              attendanceRecords[attendanceCount].location = selectedLocation;
              attendanceRecords[attendanceCount].isActive = true;
              attendanceRecords[attendanceCount].remark = remark;
              attendanceRecords[attendanceCount].duration = 0;
              attendanceCount++;
              
              // Auto-save immediately
              saveAttendance();
              
              String remarkText = getRemarkText(remark);
              currentScanResult = "{\"scanned\":true,\"name\":\"" + String(student->name) + 
                                 "\",\"grade\":\"" + String(student->grade) + 
                                 "\",\"section\":\"" + String(student->section) + 
                                 "\",\"location\":\"" + getLocationName(selectedLocation) +
                                 "\",\"time\":\"" + String(timeStr) + 
                                 "\",\"type\":\"Time In\"" +
                                 ",\"remark\":\"" + remarkText + "\"}";
              
              lcd.clear();
              lcd.print(student->name);
              lcd.setCursor(0, 1);
              lcd.print("IN ");
              lcd.print(timeStr);
              lcd.print(" ");
              lcd.print(remarkText.substring(0, 4));
              
              Serial.printf("âœ… Time In recorded for %s at %s\n", student->name, timeStr);
            }
          }
          
          delay(800);
          lcd.clear();
          lcd.print("System Ready!");
          lcd.setCursor(0, 1);
          lcd.print("Place finger...");
          
        } else {
          lcd.clear();
          lcd.print("Not Registered!");
          delay(600);
          lcd.clear();
          lcd.print("System Ready!");
          lcd.setCursor(0, 1);
          lcd.print("Place finger...");
        }
      } else {
        lcd.clear();
        lcd.print("No Match!");
        delay(400);
        lcd.clear();
        lcd.print("System Ready!");
        lcd.setCursor(0, 1);
        lcd.print("Place finger...");
      }
    }
  }
}

// ============= GET REMARK TEXT =============
String getRemarkText(int remark) {
  switch(remark) {
    case 0: return "On Time";
    case 1: return "Late";
    case 2: return "Early";
    case 3: return "Overtime";
    case 4: return "Normal";
    default: return "Unknown";
  }
}

// ============= SIMPLIFIED REGISTRATION FUNCTION =============
void handleRegistration() {
  // Check timeout
  if (millis() - registrationStartTime > 60000) {
    Serial.println("â° Registration timeout");
    lcd.clear();
    lcd.print("Timeout!");
    lcd.setCursor(0, 1);
    lcd.print("Try again");
    delay(2000);
    registrationMode = false;
    lcd.clear();
    lcd.print("System Ready!");
    lcd.setCursor(0, 1);
    lcd.print("Place finger...");
    return;
  }
  
  // Simple state machine with longer delays
  switch(registrationStep) {
    case 1: // Wait for first finger
      {
        int p = finger.getImage();
        if (p == FINGERPRINT_OK) {
          Serial.println("âœ… First image captured");
          lcd.clear();
          lcd.print("Processing...");
          delay(500);
          
          p = finger.image2Tz(1);
          if (p == FINGERPRINT_OK) {
            Serial.println("âœ… First image converted");
            lcd.clear();
            lcd.print("Remove finger");
            lcd.setCursor(0, 1);
            lcd.print("Now");
            registrationStep = 2;
            delay(2000);
          } else {
            Serial.print("âŒ Conversion failed: ");
            Serial.println(getErrorMessage(p));
            lcd.clear();
            lcd.print("Bad fingerprint");
            delay(2000);
            // Stay in step 1
          }
        }
        break;
      }
      
    case 2: // Wait for finger removal
      {
        int p = finger.getImage();
        if (p == FINGERPRINT_NOFINGER) {
          Serial.println("âœ… Finger removed");
          lcd.clear();
          lcd.print("Place same");
          lcd.setCursor(0, 1);
          lcd.print("finger again");
          registrationStep = 3;
          delay(2000);
        }
        break;
      }
      
    case 3: // Wait for second finger
      {
        int p = finger.getImage();
        if (p == FINGERPRINT_OK) {
          Serial.println("âœ… Second image captured");
          lcd.clear();
          lcd.print("Processing...");
          delay(500);
          
          p = finger.image2Tz(2);
          if (p == FINGERPRINT_OK) {
            Serial.println("âœ… Second image converted");
            lcd.clear();
            lcd.print("Creating model...");
            delay(500);
            
            // Create the model
            p = finger.createModel();
            Serial.print("Create model result: ");
            Serial.println(getErrorMessage(p));
            
            if (p == FINGERPRINT_OK) {
              Serial.println("âœ… Model created");
              
              // Check if we have space
              if (studentCount >= MAX_STUDENTS) {
                lcd.clear();
                lcd.print("Max Students!");
                delay(2000);
                registrationMode = false;
                lcd.clear();
                lcd.print("System Ready!");
                lcd.setCursor(0, 1);
                lcd.print("Place finger...");
                return;
              }
              
              // Store the model
              p = finger.storeModel(pendingId);
              Serial.print("Store model result: ");
              Serial.println(getErrorMessage(p));
              
              if (p == FINGERPRINT_OK) {
                // Save student
                students[studentCount].id = pendingId;
                pendingName.toCharArray(students[studentCount].name, 20);
                pendingGrade.toCharArray(students[studentCount].grade, 5);
                pendingSection.toCharArray(students[studentCount].section, 9);
                
                students[studentCount].name[19] = '\0';
                students[studentCount].grade[4] = '\0';
                students[studentCount].section[8] = '\0';
                students[studentCount].isRegistered = true;
                studentCount++;
                
                // Auto-save immediately
                saveStudents();
                buildFingerprintCache();
                rebuildStudentCache();
                
                lastSuccessId = pendingId;
                
                lcd.clear();
                lcd.print("Registered!");
                lcd.setCursor(0, 1);
                lcd.print("ID: ");
                lcd.print(pendingId);
                
                Serial.println("âœ… Registration SUCCESS!");
                Serial.print("   Name: ");
                Serial.print(pendingName);
                Serial.print(", ID: ");
                Serial.println(pendingId);
                Serial.print("   Total: ");
                Serial.println(studentCount);
                
                delay(3000);
                
                // Reset all registration state
                registrationMode = false;
                pendingName = "";
                pendingGrade = "";
                pendingSection = "";
                pendingId = 0;
                registrationStep = 1;
                
                lcd.clear();
                lcd.print("System Ready!");
                lcd.setCursor(0, 1);
                lcd.print("Place finger...");
                return;
              } else {
                Serial.println("âŒ Store failed");
                lcd.clear();
                lcd.print("Save failed!");
                lcd.setCursor(0, 1);
                lcd.print(getErrorMessage(p));
                delay(2000);
                
                // Clean up
                finger.deleteModel(pendingId);
                registrationMode = false;
                lcd.clear();
                lcd.print("System Ready!");
                lcd.setCursor(0, 1);
                lcd.print("Place finger...");
                return;
              }
            } else {
              Serial.println("âŒ Model creation failed");
              lcd.clear();
              if (p == FINGERPRINT_ENROLLMISMATCH) {
                lcd.print("Fingers don't");
                lcd.setCursor(0, 1);
                lcd.print("match! Try again");
              } else {
                lcd.print("Failed!");
                lcd.setCursor(0, 1);
                lcd.print(getErrorMessage(p));
              }
              delay(3000);
              
              // Reset to start
              registrationStep = 1;
              lcd.clear();
              lcd.print("Place finger");
              lcd.setCursor(0, 1);
              lcd.print("to start over");
              delay(2000);
            }
          } else {
            Serial.print("âŒ Second conversion failed: ");
            Serial.println(getErrorMessage(p));
            lcd.clear();
            lcd.print("Bad fingerprint");
            lcd.setCursor(0, 1);
            lcd.print("Try again");
            delay(2000);
          }
        }
        break;
      }
  }
}

// ============= SIMPLE RESET REGISTRATION =============
void resetRegistration() {
  registrationMode = false;
  pendingName = "";
  pendingGrade = "";
  pendingSection = "";
  pendingId = 0;
  registrationStep = 1; // Reset to step 1
  registrationStartTime = 0;
  
  // Don't touch lastSuccessId here
  
  Serial.println("ðŸ”„ Registration reset");
  
  lcd.clear();
  lcd.print("System Ready!");
  lcd.setCursor(0, 1);
  lcd.print("Place finger...");
}

// ============= GET ERROR MESSAGE =============
String getErrorMessage(uint8_t errorCode) {
  switch(errorCode) {
    case FINGERPRINT_OK:
      return "OK";
    case FINGERPRINT_PACKETRECIEVEERR:
      return "Communication error";
    case FINGERPRINT_NOFINGER:
      return "No finger detected";
    case FINGERPRINT_IMAGEFAIL:
      return "Image capture failed";
    case FINGERPRINT_IMAGEMESS:
      return "Image too messy";
    case FINGERPRINT_FEATUREFAIL:
      return "Feature extraction failed";
    case FINGERPRINT_NOMATCH:
      return "No match found";
    case FINGERPRINT_NOTFOUND:
      return "Fingerprint not found";
    case FINGERPRINT_ENROLLMISMATCH:
      return "Fingers don't match";
    case FINGERPRINT_BADLOCATION:
      return "Bad memory location";
    case FINGERPRINT_DBRANGEFAIL:
      return "Database range error";
    case FINGERPRINT_UPLOADFEATUREFAIL:
      return "Upload feature failed";
    case FINGERPRINT_PACKETRESPONSEFAIL:
      return "Packet response failed";
    case FINGERPRINT_UPLOADFAIL:
      return "Upload failed";
    case FINGERPRINT_DELETEFAIL:
      return "Delete failed";
    case FINGERPRINT_DBCLEARFAIL:
      return "Database clear failed";
    case FINGERPRINT_PASSFAIL:
      return "Password failed";
    case FINGERPRINT_INVALIDIMAGE:
      return "Invalid image";
    case FINGERPRINT_FLASHERR:
      return "Flash memory error";
    default:
      return "Unknown error: " + String(errorCode);
  }
}

// ============= CHECK SENSOR MEMORY =============
void checkSensorMemory() {
  Serial.println("\n=== SENSOR MEMORY CHECK ===");
  finger.getTemplateCount();
  Serial.print("Templates stored in sensor: ");
  Serial.println(finger.templateCount);
  
  if (finger.templateCount > 0) {
    Serial.println("Stored IDs:");
    int found = 0;
    for (int id = 1; id <= 200 && found < finger.templateCount; id++) {
      if (finger.loadModel(id) == FINGERPRINT_OK) {
        Serial.print("  ID: ");
        Serial.println(id);
        found++;
      }
    }
  }
  Serial.println("===========================\n");
}

void checkSensorCapacity() {
  finger.getTemplateCount();
  Serial.print("ðŸ“Š Sensor capacity: ");
  Serial.print(finger.templateCount);
  Serial.print("/");
  Serial.println(MAX_FINGERPRINT_CAPACITY);
  if (finger.templateCount >= MAX_FINGERPRINT_CAPACITY - 10) {
    Serial.println("âš ï¸ WARNING: Sensor almost full!");
  }
}

// ============= HELPER FUNCTIONS =============
void showMainMenu() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready!");
  lcd.setCursor(0, 1);
  lcd.print("Place finger...");
}

String getLocationName(int location) {
  if (location >= 0 && location < LOCATION_COUNT) {
    return String(locationNames[location]);
  }
  return "Unknown";
}

void buildFingerprintCache() {
  memset(fingerprintToStudentId, 0, sizeof(fingerprintToStudentId));
  for (int i = 0; i < studentCount; i++) {
    if (students[i].id < 200) {
      fingerprintToStudentId[students[i].id] = students[i].id;
    }
  }
}

// ============= SIMPLE GET NEXT AVAILABLE ID =============
int getNextAvailableId() {
  // Check for gaps in the 1-50 range
  for (int id = 1; id <= MAX_STUDENTS + 10; id++) {
    bool foundInDB = false;
    
    // Check in database only
    for (int i = 0; i < studentCount; i++) {
      if (students[i].id == id) {
        foundInDB = true;
        break;
      }
    }
    
    if (!foundInDB) {
      Serial.print("âœ… Found available ID: ");
      Serial.println(id);
      return id;
    }
  }
  
  // Fallback
  int newId = studentCount + 1;
  Serial.print("âš ï¸ Using fallback ID: ");
  Serial.println(newId);
  return newId;
}

Student* findStudentById(int id) {
  for (int i = 0; i < studentCount; i++) {
    if (students[i].id == id) return &students[i];
  }
  return NULL;
}

bool isCheckedInToday(int studentId) {
  DateTime now;
  if (!validateAndFixRTC()) {
    now = DateTime(F(__DATE__), F(__TIME__));
  } else {
    now = rtc.now();
  }
  
  char today[11];
  safeDateFormat(today, now.year(), now.month(), now.day());
  
  for (int i = attendanceCount-1; i >= max(0, attendanceCount-20); i--) {
    if (attendanceRecords[i].studentId == studentId && 
        strcmp(attendanceRecords[i].date, today) == 0 &&
        strlen(attendanceRecords[i].timeOut) == 0) {
      return true;
    }
  }
  return false;
}

void recordAttendance(int studentId, bool isTimeIn, int location) {
  DateTime now;
  if (!validateAndFixRTC()) {
    now = DateTime(F(__DATE__), F(__TIME__));
  } else {
    now = rtc.now();
  }
  
  char timeStr[12];
  safeTimeFormat(timeStr, now.hour(), now.minute(), now.second());
  
  char today[11];
  safeDateFormat(today, now.year(), now.month(), now.day());
  
  if (isTimeIn) {
    if (attendanceCount < MAX_ATTENDANCE) {
      int remark = calculateRemark(now, true);
      
      attendanceRecords[attendanceCount].studentId = studentId;
      strcpy(attendanceRecords[attendanceCount].timeIn, timeStr);
      strcpy(attendanceRecords[attendanceCount].timeOut, "");
      strcpy(attendanceRecords[attendanceCount].date, today);
      attendanceRecords[attendanceCount].location = location;
      attendanceRecords[attendanceCount].isActive = true;
      attendanceRecords[attendanceCount].remark = remark;
      attendanceRecords[attendanceCount].duration = 0;
      attendanceCount++;
      saveAttendance();
      Serial.printf("âœ… Time In recorded for ID %d at %s (%s)\n", studentId, locationNames[location], getRemarkText(remark).c_str());
    }
  } else {
    for (int i = attendanceCount-1; i >= max(0, attendanceCount-20); i--) {
      if (attendanceRecords[i].studentId == studentId && 
          strlen(attendanceRecords[i].timeOut) == 0) {
        strcpy(attendanceRecords[i].timeOut, timeStr);
        attendanceRecords[i].isActive = false;
        attendanceRecords[i].remark = calculateRemark(now, false);
        attendanceRecords[i].duration = calculateDuration(attendanceRecords[i].timeIn, attendanceRecords[i].timeOut);
        saveAttendance();
        Serial.printf("âœ… Time Out recorded for ID %d at %s (%s)\n", studentId, locationNames[location], getRemarkText(attendanceRecords[i].remark).c_str());
        break;
      }
    }
  }
}

// ============= AI ASSISTANT (Offline / Cloud / Local) =============
// Three interchangeable modes, selected in the Assistant settings panel:
//   0 = Offline  - keyword rules below, zero network dependency
//   1 = Cloud AI - real LLM via the internet (Anthropic Messages API)
//   2 = Local AI - real LLM on a LAN device (e.g. Ollama on a Raspberry Pi)
// Cloud/Local both send a text snapshot of the current attendance data
// (buildAttendanceContext) alongside the user's question, so the model can
// answer using this device's actual data rather than guessing.

void loadAssistantSettings() {
  assistantPrefs.begin("assistant", true); // read-only
  assistantSettings.mode = assistantPrefs.getUChar("mode", 0);
  assistantSettings.wifiSSID = assistantPrefs.getString("wifiSSID", "");
  assistantSettings.wifiPassword = assistantPrefs.getString("wifiPass", "");
  assistantSettings.apiKey = assistantPrefs.getString("apiKey", "");
  assistantSettings.apiEndpoint = assistantPrefs.getString("apiEndpoint", "https://api.anthropic.com/v1/messages");
  assistantSettings.apiModel = assistantPrefs.getString("apiModel", "claude-haiku-4-5-20251001");
  assistantSettings.localURL = assistantPrefs.getString("localURL", "");
  assistantSettings.localModel = assistantPrefs.getString("localModel", "llama3.2");
  assistantPrefs.end();
}

void saveAssistantSettings() {
  assistantPrefs.begin("assistant", false); // read-write
  assistantPrefs.putUChar("mode", assistantSettings.mode);
  assistantPrefs.putString("wifiSSID", assistantSettings.wifiSSID);
  assistantPrefs.putString("wifiPass", assistantSettings.wifiPassword);
  assistantPrefs.putString("apiKey", assistantSettings.apiKey);
  assistantPrefs.putString("apiEndpoint", assistantSettings.apiEndpoint);
  assistantPrefs.putString("apiModel", assistantSettings.apiModel);
  assistantPrefs.putString("localURL", assistantSettings.localURL);
  assistantPrefs.putString("localModel", assistantSettings.localModel);
  assistantPrefs.end();
}

// Joins the configured router WiFi (in addition to the self-hosted AP) so
// the device has internet access for Cloud AI. Only used in mode 1. The
// self-hosted AP is untouched either way - this never breaks the offline
// dashboard. Returns true if connected within the timeout.
bool ensureInternetConnected() {
  if (assistantSettings.wifiSSID.length() == 0) return false;
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_AP_STA); // keep the self-hosted AP alive while also joining a router
  WiFi.begin(assistantSettings.wifiSSID.c_str(), assistantSettings.wifiPassword.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Escapes a string for safe embedding inside a JSON string value.
String jsonEscape(String input) {
  String out = "";
  for (unsigned int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') { /* skip */ }
    else out += c;
  }
  return out;
}

// Minimal extractor for {"key":"value"} pairs, with basic unescaping.
// Not a full JSON parser - deliberately simple, matching this project's
// existing manual-JSON style, and good enough for the response shapes
// Anthropic/Ollama send back.
String extractJsonStringValue(String json, String key) {
  String needle = "\"" + key + "\":\"";
  int start = json.indexOf(needle);
  if (start < 0) return "";
  start += needle.length();

  String out = "";
  for (unsigned int i = start; i < json.length(); i++) {
    char c = json.charAt(i);
    if (c == '\\' && i + 1 < json.length()) {
      char next = json.charAt(i + 1);
      if (next == 'n') { out += '\n'; i++; continue; }
      if (next == '"') { out += '"'; i++; continue; }
      if (next == '\\') { out += '\\'; i++; continue; }
      continue;
    }
    if (c == '"') break; // end of value
    out += c;
  }
  return out;
}

// Builds a compact text snapshot of today's attendance so a real LLM can
// answer accurately about THIS device's data instead of guessing.
String buildAttendanceContext() {
  DateTime now;
  if (!validateAndFixRTC()) {
    now = DateTime(F(__DATE__), F(__TIME__));
  } else {
    now = rtc.now();
  }
  char todayBuf[11];
  safeDateFormat(todayBuf, now.year(), now.month(), now.day());

  String ctx = "Today's date: " + String(todayBuf) + "\n";
  ctx += "Total registered students: " + String(studentCount) + "\n\n";

  ctx += "Today's attendance records (name, location, time in, time out, status):\n";
  int todayRecords = 0;
  for (int i = 0; i < attendanceCount; i++) {
    if (strcmp(attendanceRecords[i].date, todayBuf) == 0) {
      Student* s = findStudentById(attendanceRecords[i].studentId);
      String name = (s != NULL) ? String(s->name) : ("Unknown#" + String(attendanceRecords[i].studentId));
      ctx += "- " + name + ", " + getLocationName(attendanceRecords[i].location) +
             ", in " + String(attendanceRecords[i].timeIn) +
             ", out " + (strlen(attendanceRecords[i].timeOut) ? String(attendanceRecords[i].timeOut) : String("still inside")) +
             ", " + getRemarkText(attendanceRecords[i].remark) + "\n";
      todayRecords++;
    }
  }
  if (todayRecords == 0) ctx += "(none yet)\n";

  ctx += "\nFull roster (name, grade, section):\n";
  for (int i = 0; i < studentCount; i++) {
    ctx += "- " + String(students[i].name) + ", grade " + String(students[i].grade) +
           ", section " + String(students[i].section) + "\n";
  }

  return ctx;
}

// ---------- CLOUD AI (Anthropic Messages API over the internet) ----------
String getCloudAIAnswer(String query) {
  if (assistantSettings.apiKey.length() == 0) return "";
  if (!ensureInternetConnected()) return "";

  WiFiClientSecure client;
  client.setInsecure(); // NOTE: skips certificate validation to keep this
                         // sketch simple/dependency-free. Fine on a trusted
                         // network; for stricter security, pin Anthropic's
                         // root certificate instead.

  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(client, assistantSettings.apiEndpoint)) return "";

  http.addHeader("content-type", "application/json");
  http.addHeader("x-api-key", assistantSettings.apiKey);
  http.addHeader("anthropic-version", "2023-06-01");

  String system = "You are an offline attendance assistant for a school kiosk. "
                  "Answer briefly (2-4 sentences) using ONLY the attendance data "
                  "provided below. If the data doesn't answer the question, say so.\n\n" +
                  buildAttendanceContext();

  String body = "{";
  body += "\"model\":\"" + assistantSettings.apiModel + "\",";
  body += "\"max_tokens\":300,";
  body += "\"system\":\"" + jsonEscape(system) + "\",";
  body += "\"messages\":[{\"role\":\"user\",\"content\":\"" + jsonEscape(query) + "\"}]";
  body += "}";

  int code = http.POST(body);
  String answer = "";
  if (code == 200) {
    String response = http.getString();
    answer = extractJsonStringValue(response, "text");
  } else {
    Serial.print("Cloud AI request failed, HTTP code: ");
    Serial.println(code);
  }
  http.end();
  return answer;
}

// ---------- LOCAL AI (LAN device running an LLM server, e.g. Ollama) ----------
String getLocalAIAnswer(String query) {
  if (assistantSettings.localURL.length() == 0) return "";

  HTTPClient http;
  http.setTimeout(15000);
  String url = assistantSettings.localURL;
  if (!url.endsWith("/")) url += "/";
  url += "api/generate"; // Ollama-compatible endpoint

  if (!http.begin(url)) return "";
  http.addHeader("content-type", "application/json");

  String prompt = "You are an offline attendance assistant for a school kiosk. "
                   "Answer briefly (2-4 sentences) using ONLY this data:\n\n" +
                   buildAttendanceContext() + "\nQuestion: " + query;

  String body = "{";
  body += "\"model\":\"" + assistantSettings.localModel + "\",";
  body += "\"prompt\":\"" + jsonEscape(prompt) + "\",";
  body += "\"stream\":false";
  body += "}";

  int code = http.POST(body);
  String answer = "";
  if (code == 200) {
    String response = http.getString();
    answer = extractJsonStringValue(response, "response");
  } else {
    Serial.print("Local AI request failed, HTTP code: ");
    Serial.println(code);
  }
  http.end();
  return answer;
}

// ============= OFFLINE ASSISTANT RULES (fallback / mode 0) =============

String assistantToday(char* buffer) {
  DateTime now;
  if (!validateAndFixRTC()) {
    now = DateTime(F(__DATE__), F(__TIME__));
  } else {
    now = rtc.now();
  }
  safeDateFormat(buffer, now.year(), now.month(), now.day());
  return String(buffer);
}

String processAssistantQuery(String query) {
  String q = query;
  q.toLowerCase();
  q.trim();

  if (q.length() == 0) {
    return "Ask me something about attendance â€” try \"help\" to see what I can do.";
  }

  char todayBuf[11];
  String today = assistantToday(todayBuf);

  // ---------- HELP ----------
  if (q.indexOf("help") >= 0 || q == "?" || q.indexOf("what can you") >= 0) {
    return "I can answer things like:\n"
           "- Who is late today?\n"
           "- Who is inside right now?\n"
           "- Who is absent today?\n"
           "- How many students today?\n"
           "- Total students\n"
           "- Is <name> here?\n"
           "- When did <name> check in?\n"
           "- Students in grade <x> / section <x>\n"
           "- List locations";
  }

  // ---------- LOCATIONS ----------
  if (q.indexOf("location") >= 0 && (q.indexOf("list") >= 0 || q.indexOf("what") >= 0 || q.indexOf("which") >= 0)) {
    String out = "Locations: ";
    for (int i = 0; i < LOCATION_COUNT; i++) {
      out += String(locationNames[i]);
      if (i < LOCATION_COUNT - 1) out += ", ";
    }
    return out;
  }

  // ---------- TOTAL STUDENTS ----------
  if (q.indexOf("total student") >= 0 || q.indexOf("how many student") >= 0) {
    // "how many students today" should fall through to attendance count instead
    if (q.indexOf("today") < 0) {
      return "There " + String(studentCount == 1 ? "is " : "are ") + String(studentCount) +
             " registered student" + String(studentCount == 1 ? "" : "s") + ".";
    }
  }

  // ---------- WHO IS LATE TODAY ----------
  if (q.indexOf("late") >= 0) {
    String names = "";
    int count = 0;
    for (int i = 0; i < attendanceCount; i++) {
      if (strcmp(attendanceRecords[i].date, todayBuf) == 0 && attendanceRecords[i].remark == 1) {
        Student* s = findStudentById(attendanceRecords[i].studentId);
        if (s != NULL) {
          if (count > 0) names += ", ";
          names += String(s->name);
          count++;
        }
      }
    }
    if (count == 0) return "Nobody has been marked late today.";
    return String(count) + " student" + String(count == 1 ? " was" : "s were") + " late today: " + names;
  }

  // ---------- WHO IS INSIDE / ACTIVE NOW ----------
  if (q.indexOf("inside") >= 0 || q.indexOf("active now") >= 0 || q.indexOf("currently") >= 0 ||
      (q.indexOf("who") >= 0 && q.indexOf("in") >= 0 && q.indexOf("out") < 0 && q.indexOf("today") < 0 && q.indexOf("absent") < 0)) {
    String names = "";
    int count = 0;
    for (int i = 0; i < attendanceCount; i++) {
      if (strcmp(attendanceRecords[i].date, todayBuf) == 0 && attendanceRecords[i].isActive) {
        Student* s = findStudentById(attendanceRecords[i].studentId);
        if (s != NULL) {
          if (count > 0) names += ", ";
          names += String(s->name) + " (" + getLocationName(attendanceRecords[i].location) + ")";
          count++;
        }
      }
    }
    if (count == 0) return "Nobody is currently checked in.";
    return String(count) + " student" + String(count == 1 ? " is" : "s are") + " currently inside: " + names;
  }

  // ---------- WHO IS ABSENT TODAY ----------
  if (q.indexOf("absent") >= 0) {
    String names = "";
    int count = 0;
    for (int i = 0; i < studentCount; i++) {
      bool seenToday = false;
      for (int j = 0; j < attendanceCount; j++) {
        if (attendanceRecords[j].studentId == students[i].id && strcmp(attendanceRecords[j].date, todayBuf) == 0) {
          seenToday = true;
          break;
        }
      }
      if (!seenToday) {
        if (count > 0) names += ", ";
        names += String(students[i].name);
        count++;
      }
    }
    if (count == 0) return "Everyone has been recorded today â€” no absences.";
    return String(count) + " student" + String(count == 1 ? " has" : "s have") + " not checked in today: " + names;
  }

  // ---------- ATTENDANCE COUNT TODAY ----------
  if (q.indexOf("how many") >= 0 && (q.indexOf("today") >= 0 || q.indexOf("attendance") >= 0)) {
    int todayCount = 0;
    for (int i = 0; i < attendanceCount; i++) {
      if (strcmp(attendanceRecords[i].date, todayBuf) == 0) todayCount++;
    }
    return String(todayCount) + " attendance record" + String(todayCount == 1 ? "" : "s") + " logged today (" + today + ").";
  }

  // ---------- GRADE / SECTION LOOKUP ----------
  if (q.indexOf("grade") >= 0 || q.indexOf("section") >= 0) {
    String out = "";
    int count = 0;
    for (int i = 0; i < studentCount; i++) {
      String grade = String(students[i].grade);
      String section = String(students[i].section);
      String gradeLower = grade; gradeLower.toLowerCase();
      String sectionLower = section; sectionLower.toLowerCase();
      if ((q.indexOf(gradeLower) >= 0 && gradeLower.length() > 0) ||
          (q.indexOf(sectionLower) >= 0 && sectionLower.length() > 0)) {
        if (count > 0) out += ", ";
        out += String(students[i].name);
        count++;
      }
    }
    if (count > 0) return String(count) + " matching student" + String(count == 1 ? "" : "s") + ": " + out;
    return "I couldn't find a grade or section matching that in the roster.";
  }

  // ---------- SPECIFIC STUDENT: "is <name> here" / "when did <name> check in" ----------
  for (int i = 0; i < studentCount; i++) {
    String name = String(students[i].name);
    String nameLower = name; nameLower.toLowerCase();
    if (nameLower.length() > 0 && q.indexOf(nameLower) >= 0) {
      // find today's record for this student
      bool found = false;
      for (int j = attendanceCount - 1; j >= 0; j--) {
        if (attendanceRecords[j].studentId == students[i].id && strcmp(attendanceRecords[j].date, todayBuf) == 0) {
          found = true;
          if (strlen(attendanceRecords[j].timeOut) == 0) {
            return name + " checked in today at " + String(attendanceRecords[j].timeIn) +
                   " (" + getLocationName(attendanceRecords[j].location) + ") and is still inside â€” " +
                   getRemarkText(attendanceRecords[j].remark) + ".";
          } else {
            return name + " checked in at " + String(attendanceRecords[j].timeIn) +
                   " and checked out at " + String(attendanceRecords[j].timeOut) + " today.";
          }
        }
      }
      if (!found) {
        return name + " has not checked in today.";
      }
    }
  }

  return "I'm not sure how to answer that yet. Try \"help\" to see example questions I understand.";
}

// ============= EEPROM FUNCTIONS =============
void saveStudents() {
  int addr = STUDENT_COUNT_ADDR;
  
  if (studentCount < 0 || studentCount > MAX_STUDENTS) {
    Serial.println("âŒ Invalid student count, resetting to 0");
    studentCount = 0;
  }
  
  EEPROM.writeInt(addr, studentCount);
  addr = STUDENT_DATA_START;
  
  Serial.printf("ðŸ’¾ Saving %d students to EEPROM\n", studentCount);
  
  for (int i = 0; i < studentCount; i++) {
    EEPROM.writeInt(addr, students[i].id);
    addr += sizeof(int);
    
    EEPROM.writeBytes(addr, students[i].name, 20);
    addr += 20;
    
    EEPROM.writeBytes(addr, students[i].grade, 5);
    addr += 5;
    
    EEPROM.writeBytes(addr, students[i].section, 9);
    addr += 9;
    
    EEPROM.writeBool(addr, students[i].isRegistered);
    addr += sizeof(bool);
    
    int padding = STUDENT_RECORD_SIZE - (sizeof(int) + 20 + 5 + 9 + sizeof(bool));
    for (int j = 0; j < padding; j++) {
      EEPROM.write(addr + j, 0);
    }
    addr += padding;
  }
  
  if (EEPROM.commit()) {
    Serial.println("âœ… EEPROM commit successful");
  } else {
    Serial.println("âŒ EEPROM commit failed!");
  }
}

void loadStudents() {
  int addr = STUDENT_COUNT_ADDR;
  studentCount = EEPROM.readInt(addr);
  
  Serial.printf("ðŸ“‚ Reading student count from EEPROM: %d\n", studentCount);
  
  if (studentCount < 0 || studentCount > MAX_STUDENTS) {
    Serial.printf("âš ï¸ Invalid student count: %d, resetting to 0\n", studentCount);
    studentCount = 0;
    return;
  }
  
  addr = STUDENT_DATA_START;
  
  for (int i = 0; i < studentCount; i++) {
    students[i].id = EEPROM.readInt(addr);
    addr += sizeof(int);
    
    EEPROM.readBytes(addr, students[i].name, 20);
    students[i].name[19] = '\0';
    addr += 20;
    
    EEPROM.readBytes(addr, students[i].grade, 5);
    students[i].grade[4] = '\0';
    addr += 5;
    
    EEPROM.readBytes(addr, students[i].section, 9);
    students[i].section[8] = '\0';
    addr += 9;
    
    students[i].isRegistered = EEPROM.readBool(addr);
    addr += sizeof(bool);
    
    int padding = STUDENT_RECORD_SIZE - (sizeof(int) + 20 + 5 + 9 + sizeof(bool));
    addr += padding;
    
    if (students[i].id < 1 || students[i].id > 1000) {
      Serial.printf("âš ï¸ Invalid student ID %d at index %d, stopping load\n", students[i].id, i);
      studentCount = i;
      break;
    }
  }
  
  Serial.printf("ðŸ“‚ Successfully loaded %d students from EEPROM\n", studentCount);
}

void saveAttendance() {
  int addr = ATTENDANCE_COUNT_ADDR;
  
  if (attendanceCount < 0 || attendanceCount > MAX_ATTENDANCE) {
    attendanceCount = 0;
  }
  
  EEPROM.writeInt(addr, attendanceCount);
  addr = ATTENDANCE_DATA_START;
  
  for (int i = 0; i < attendanceCount; i++) {
    EEPROM.writeInt(addr, attendanceRecords[i].studentId);
    addr += sizeof(int);
    
    EEPROM.writeBytes(addr, attendanceRecords[i].timeIn, 12);
    addr += 12;
    
    EEPROM.writeBytes(addr, attendanceRecords[i].timeOut, 12);
    addr += 12;
    
    EEPROM.writeBytes(addr, attendanceRecords[i].date, 11);
    addr += 11;
    
    EEPROM.writeInt(addr, attendanceRecords[i].location);
    addr += sizeof(int);
    
    EEPROM.writeBool(addr, attendanceRecords[i].isActive);
    addr += sizeof(bool);
    
    EEPROM.writeInt(addr, attendanceRecords[i].remark);
    addr += sizeof(int);
    
    EEPROM.writeInt(addr, attendanceRecords[i].duration);
    addr += sizeof(int);
    
    int padding = ATTENDANCE_RECORD_SIZE - (sizeof(int) + 12 + 12 + 11 + sizeof(int) + sizeof(bool) + sizeof(int) + sizeof(int));
    for (int j = 0; j < padding; j++) {
      EEPROM.write(addr + j, 0);
    }
    addr += padding;
  }
  
  EEPROM.commit();
  Serial.printf("ðŸ’¾ Saved %d attendance records\n", attendanceCount);
}

void loadAttendance() {
  int addr = ATTENDANCE_COUNT_ADDR;
  attendanceCount = EEPROM.readInt(addr);
  
  if (attendanceCount < 0 || attendanceCount > MAX_ATTENDANCE) {
    attendanceCount = 0;
    return;
  }
  
  addr = ATTENDANCE_DATA_START;
  
  for (int i = 0; i < attendanceCount; i++) {
    attendanceRecords[i].studentId = EEPROM.readInt(addr);
    addr += sizeof(int);
    
    EEPROM.readBytes(addr, attendanceRecords[i].timeIn, 12);
    attendanceRecords[i].timeIn[11] = '\0';
    addr += 12;
    
    EEPROM.readBytes(addr, attendanceRecords[i].timeOut, 12);
    attendanceRecords[i].timeOut[11] = '\0';
    addr += 12;
    
    EEPROM.readBytes(addr, attendanceRecords[i].date, 11);
    attendanceRecords[i].date[10] = '\0';
    addr += 11;
    
    attendanceRecords[i].location = EEPROM.readInt(addr);
    addr += sizeof(int);
    
    attendanceRecords[i].isActive = EEPROM.readBool(addr);
    addr += sizeof(bool);
    
    attendanceRecords[i].remark = EEPROM.readInt(addr);
    addr += sizeof(int);
    
    attendanceRecords[i].duration = EEPROM.readInt(addr);
    addr += sizeof(int);
    
    int padding = ATTENDANCE_RECORD_SIZE - (sizeof(int) + 12 + 12 + 11 + sizeof(int) + sizeof(bool) + sizeof(int) + sizeof(int));
    addr += padding;
  }
  
  Serial.printf("ðŸ“‚ Loaded %d attendance records\n", attendanceCount);
}