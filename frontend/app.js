// ===========================
// Configuration
// ===========================
let ESP32_IP = '192.168.4.1';
let ws;
let reconnectInterval = 3000;
let packetCount = 0;

// ===========================
// Initialize
// ===========================
document.addEventListener('DOMContentLoaded', () => {
    console.log('Dashboard loaded');
    // Load saved IP from localStorage if exists
    const savedIP = localStorage.getItem('esp32_ip');
    if (savedIP) {
        ESP32_IP = savedIP;
        document.getElementById('espIP').value = savedIP;
    }
    connectToESP();
});

// ===========================
// Connect to ESP32
// ===========================
function connectToESP() {
    const ipInput = document.getElementById('espIP').value;
    if (ipInput) {
        ESP32_IP = ipInput;
        localStorage.setItem('esp32_ip', ESP32_IP);
    }
    
    console.log('Connecting to ESP32 at:', ESP32_IP);
    connectWebSocket();
}

// ===========================
// WebSocket Connection
// ===========================
function connectWebSocket() {
    const wsUrl = `ws://${ESP32_IP}/ws`;
    console.log('WebSocket URL:', wsUrl);
    
    try {
        ws = new WebSocket(wsUrl);
        
        ws.onopen = function() {
            console.log('✅ WebSocket connected');
            updateConnectionStatus(true);
        };
        
        ws.onclose = function() {
            console.log('❌ WebSocket disconnected');
            updateConnectionStatus(false);
            setTimeout(connectWebSocket, reconnectInterval);
        };
        
        ws.onerror = function(error) {
            console.error('WebSocket error:', error);
            updateConnectionStatus(false);
        };
        
        ws.onmessage = function(event) {
            try {
                const data = JSON.parse(event.data);
                console.log('📦 Data received:', data);
                updateDashboard(data);
            } catch (e) {
                console.error('Error parsing data:', e);
            }
        };
    } catch (error) {
        console.error('Failed to create WebSocket:', error);
        updateConnectionStatus(false);
    }
}

// ===========================
// Update Connection Status
// ===========================
function updateConnectionStatus(connected) {
    const statusDot = document.getElementById('connectionStatus');
    const statusText = document.getElementById('connectionText');
    
    if (connected) {
        statusDot.className = 'status-dot connected';
        statusText.textContent = 'Connected';
        statusText.className = 'text-success';
    } else {
        statusDot.className = 'status-dot disconnected';
        statusText.textContent = 'Disconnected';
        statusText.className = 'text-danger';
    }
}

// ===========================
// Update Dashboard with Data
// ===========================
function updateDashboard(data) {
    // Update packet count
    packetCount++;
    document.getElementById('packetsReceived').textContent = packetCount;
    
    // Update timestamp
    const now = new Date();
    document.getElementById('lastUpdate').textContent = now.toLocaleTimeString();
    
    // Update signal quality
    if (data.rssi) {
        const rssi = data.rssi;
        const quality = getSignalQuality(rssi);
        document.getElementById('signalQuality').textContent = `${rssi} dBm (${quality})`;
    }
    
    // Update GPS data
    updateGPSData(data.gps);
    
    // Update IMU data
    updateIMUData(data.imu);
    
    // Update Navigation data (if available)
    updateNavigationData(data.nav);
}

// ===========================
// Update GPS Data
// ===========================
function updateGPSData(gps) {
    const gpsBadge = document.getElementById('gpsBadge');
    
    if (gps && gps.valid) {
        // GPS has valid fix
        gpsBadge.textContent = 'GPS Fix';
        gpsBadge.className = 'badge success';
        
        document.getElementById('gpsLat').textContent = gps.lat.toFixed(6) + '°';
        document.getElementById('gpsLng').textContent = gps.lng.toFixed(6) + '°';
        document.getElementById('gpsAlt').textContent = gps.alt.toFixed(1) + ' m';
        document.getElementById('gpsSpeed').textContent = gps.speed.toFixed(1) + ' km/h';
        document.getElementById('gpsSats').textContent = gps.sats + ' sats';
    } else {
        // No GPS fix
        gpsBadge.textContent = 'No Fix';
        gpsBadge.className = 'badge warning';
        
        document.getElementById('gpsLat').textContent = '--';
        document.getElementById('gpsLng').textContent = '--';
        document.getElementById('gpsAlt').textContent = '--';
        document.getElementById('gpsSpeed').textContent = '--';
        document.getElementById('gpsSats').textContent = '--';
    }
}

// ===========================
// Update IMU Data
// ===========================
function updateIMUData(imu) {
    const imuBadge = document.getElementById('imuBadge');
    
    if (imu && imu.valid) {
        imuBadge.textContent = 'Active';
        imuBadge.className = 'badge success';
        
        // Accelerometer
        const accel = imu.accel;
        document.getElementById('accelX').textContent = accel.x.toFixed(2) + ' m/s²';
        document.getElementById('accelY').textContent = accel.y.toFixed(2) + ' m/s²';
        document.getElementById('accelZ').textContent = accel.z.toFixed(2) + ' m/s²';
        
        // Calculate magnitude
        const magnitude = Math.sqrt(accel.x**2 + accel.y**2 + accel.z**2);
        document.getElementById('accelMag').textContent = magnitude.toFixed(2) + ' m/s²';
        
        // Gyroscope
        const gyro = imu.gyro;
        document.getElementById('gyroX').textContent = gyro.x.toFixed(2) + ' rad/s';
        document.getElementById('gyroY').textContent = gyro.y.toFixed(2) + ' rad/s';
        document.getElementById('gyroZ').textContent = gyro.z.toFixed(2) + ' rad/s';
        
        // Temperature
        if (imu.temp) {
            document.getElementById('imuTemp').textContent = imu.temp.toFixed(1) + ' °C';
        }
    } else {
        imuBadge.textContent = 'Inactive';
        imuBadge.className = 'badge warning';
        
        document.getElementById('accelX').textContent = '--';
        document.getElementById('accelY').textContent = '--';
        document.getElementById('accelZ').textContent = '--';
        document.getElementById('accelMag').textContent = '--';
        document.getElementById('gyroX').textContent = '--';
        document.getElementById('gyroY').textContent = '--';
        document.getElementById('gyroZ').textContent = '--';
        document.getElementById('imuTemp').textContent = '--';
    }
}

// ===========================
// Update Navigation Data (for haptic navigation example)
// ===========================
function updateNavigationData(nav) {
    const navCard = document.getElementById('navCard');
    
    if (nav && nav.distance !== undefined) {
        // Show navigation card if data is present
        navCard.style.display = 'block';
        
        // Distance with appropriate units
        let distanceText;
        if (nav.distance < 1000) {
            distanceText = nav.distance.toFixed(1) + ' m';
        } else {
            distanceText = (nav.distance / 1000).toFixed(2) + ' km';
        }
        document.getElementById('navDistance').textContent = distanceText;
        
        // Bearing
        document.getElementById('navBearing').textContent = nav.bearing.toFixed(1) + '°';
        
        // Heading
        document.getElementById('navHeading').textContent = nav.heading.toFixed(1) + '°';
        
        // Active motor with icon
        const motorNames = ['⬆️ FRONT', '➡️ RIGHT', '⬇️ BACK', '⬅️ LEFT', '⏸️ NONE'];
        const motorName = motorNames[nav.active_motor] || 'Unknown';
        document.getElementById('navActiveMotor').textContent = motorName;
        
        // Base station coordinates
        const baseCoords = nav.base_lat.toFixed(6) + ', ' + nav.base_lng.toFixed(6);
        document.getElementById('navBaseCoords').textContent = baseCoords;
        
        // Update badge based on distance
        const navBadge = document.getElementById('navBadge');
        if (nav.distance < 10) {
            navBadge.textContent = 'Arrived!';
            navBadge.className = 'badge success';
        } else if (nav.distance < 100) {
            navBadge.textContent = 'Close';
            navBadge.className = 'badge warning';
        } else {
            navBadge.textContent = 'Navigating';
            navBadge.className = 'badge';
        }
    } else {
        // Hide navigation card if no nav data
        navCard.style.display = 'none';
    }
}

// ===========================
// Get Signal Quality Label
// ===========================
function getSignalQuality(rssi) {
    if (rssi > -50) return 'Excellent';
    if (rssi > -60) return 'Good';
    if (rssi > -70) return 'Fair';
    if (rssi > -80) return 'Weak';
    return 'Poor';
}

// ===========================
// API Functions (Optional)
// ===========================

// Get latest data via REST API
async function fetchLatestData() {
    try {
        const response = await fetch(`http://${ESP32_IP}/api/data`);
        const data = await response.json();
        console.log('API data:', data);
        updateDashboard(data);
    } catch (error) {
        console.error('Error fetching data:', error);
    }
}

// Get history via REST API
async function fetchHistory() {
    try {
        const response = await fetch(`http://${ESP32_IP}/api/history`);
        const history = await response.json();
        console.log('History:', history);
        return history;
    } catch (error) {
        console.error('Error fetching history:', error);
        return [];
    }
}

// Get system info
async function fetchSystemInfo() {
    try {
        const response = await fetch(`http://${ESP32_IP}/api/info`);
        const info = await response.json();
        console.log('System info:', info);
        return info;
    } catch (error) {
        console.error('Error fetching system info:', error);
        return null;
    }
}

// Export to CSV (example function)
function exportToCSV() {
    // This would export historical data to CSV
    console.log('Export to CSV functionality - to be implemented');
}

// ===========================
// Keyboard Shortcuts
// ===========================
document.addEventListener('keydown', (e) => {
    // R key to reconnect
    if (e.key === 'r' || e.key === 'R') {
        console.log('Manual reconnect triggered');
        connectWebSocket();
    }
    
    // F key to fetch latest data via API
    if (e.key === 'f' || e.key === 'F') {
        console.log('Fetching latest data via API');
        fetchLatestData();
    }
});

// ===========================
// Console Welcome Message
// ===========================
console.log(`
╔══════════════════════════════════════╗
║   Belt Monitoring Dashboard v1.0     ║
║   Connected to: ${ESP32_IP}         ║
║                                      ║
║   Keyboard Shortcuts:                ║
║   R - Reconnect WebSocket            ║
║   F - Fetch latest data              ║
╚══════════════════════════════════════╝
`);
