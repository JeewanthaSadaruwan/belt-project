// ===========================
// Enhanced Belt Station Dashboard
// ===========================

let ws = null;
let reconnectInterval = null;
let map = null;
let marker = null;
let pathLine = null;
let pathCoordinates = [];
let accelChart = null;
let gyroChart = null;
let packetCount = 0;

// Chart data buffers
const maxDataPoints = 20;
const accelData = { x: [], y: [], z: [] };
const gyroData = { x: [], y: [], z: [] };

// ===========================
// Initialize on Page Load
// ===========================
document.addEventListener('DOMContentLoaded', function() {
    initMap();
    initCharts();
    connectWebSocket();
});

// ===========================
// Initialize Leaflet Map
// ===========================
function initMap() {
    // Initialize map centered on default location
    map = L.map('map').setView([37.7749, -122.4194], 13);
    
    // Use dark theme tiles
    L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', {
        attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
        subdomains: 'abcd',
        maxZoom: 19
    }).addTo(map);
    
    // Initialize marker
    marker = L.marker([37.7749, -122.4194], {
        icon: L.divIcon({
            className: 'custom-div-icon',
            html: "<div style='background-color:#8b5cf6; width: 20px; height: 20px; border-radius: 50%; border: 3px solid white;'></div>",
            iconSize: [20, 20],
            iconAnchor: [10, 10]
        })
    }).addTo(map);
    
    // Initialize path line
    pathLine = L.polyline(pathCoordinates, {
        color: '#8b5cf6',
        weight: 3,
        opacity: 0.7
    }).addTo(map);
}

// ===========================
// Initialize Charts
// ===========================
function initCharts() {
    const chartColors = {
        x: '#ef4444',      // red
        y: '#10b981',      // green  
        z: '#3b82f6'       // blue
    };
    
    const chartConfig = {
        type: 'line',
        options: {
            responsive: true,
            maintainAspectRatio: true,
            animation: {
                duration: 200
            },
            scales: {
                y: {
                    ticks: { color: '#cbd5e1' },
                    grid: { color: '#334155' }
                },
                x: {
                    ticks: { color: '#cbd5e1' },
                    grid: { color: '#334155' }
                }
            },
            plugins: {
                legend: {
                    labels: { color: '#cbd5e1' }
                }
            }
        }
    };
    
    // Accelerometer Chart
    const accelCtx = document.getElementById('accelChart').getContext('2d');
    accelChart = new Chart(accelCtx, {
        ...chartConfig,
        data: {
            labels: Array(maxDataPoints).fill(''),
            datasets: [
                {
                    label: 'X-Axis',
                    data: accelData.x,
                    borderColor: chartColors.x,
                    backgroundColor: chartColors.x + '33',
                    tension: 0.4,
                    borderWidth: 2
                },
                {
                    label: 'Y-Axis',
                    data: accelData.y,
                    borderColor: chartColors.y,
                    backgroundColor: chartColors.y + '33',
                    tension: 0.4,
                    borderWidth: 2
                },
                {
                    label: 'Z-Axis',
                    data: accelData.z,
                    borderColor: chartColors.z,
                    backgroundColor: chartColors.z + '33',
                    tension: 0.4,
                    borderWidth: 2
                }
            ]
        }
    });
    
    // Gyroscope Chart
    const gyroCtx = document.getElementById('gyroChart').getContext('2d');
    gyroChart = new Chart(gyroCtx, {
        ...chartConfig,
        data: {
            labels: Array(maxDataPoints).fill(''),
            datasets: [
                {
                    label: 'X-Axis',
                    data: gyroData.x,
                    borderColor: chartColors.x,
                    backgroundColor: chartColors.x + '33',
                    tension: 0.4,
                    borderWidth: 2
                },
                {
                    label: 'Y-Axis',
                    data: gyroData.y,
                    borderColor: chartColors.y,
                    backgroundColor: chartColors.y + '33',
                    tension: 0.4,
                    borderWidth: 2
                },
                {
                    label: 'Z-Axis',
                    data: gyroData.z,
                    borderColor: chartColors.z,
                    backgroundColor: chartColors.z + '33',
                    tension: 0.4,
                    borderWidth: 2
                }
            ]
        }
    });
}

// ===========================
// HTTP Polling
// ===========================
function connectWebSocket() {
    startPolling();
}

function startPolling() {
    fetchData(); // immediate first fetch
    setInterval(fetchData, 2000); // poll every 2 seconds
}

function fetchData() {
    fetch('/api/data')
        .then(function(response) {
            if (!response.ok) throw new Error('HTTP ' + response.status);
            return response.json();
        })
        .then(function(data) {
            // Only update if we have real data (not empty object)
            if (data && data.gps) {
                updateConnectionStatus(true);
                updateDashboard(data);
                packetCount++;
                document.getElementById('packetCount').textContent = packetCount;
                document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
            }
        })
        .catch(function(error) {
            console.error('Fetch error:', error);
            updateConnectionStatus(false);
        });
}

// ===========================
// Update Connection Status
// ===========================
function updateConnectionStatus(connected) {
    const statusEl = document.getElementById('connectionStatus');
    const statusText = statusEl.querySelector('.status-text');
    
    if (connected) {
        statusEl.classList.add('connected');
        statusText.textContent = 'Connected';
    } else {
        statusEl.classList.remove('connected');
        statusText.textContent = 'Disconnected';
    }
}

// ===========================
// Update Dashboard
// ===========================
function updateDashboard(data) {
    if (data.gps) updateGPS(data.gps);
    if (data.imu) {
        updateIMU(data.imu);
        updateGyro(data.imu);
    }
    if (data.nav) updateNavigation(data.nav);
}

// ===========================
// Update GPS Data & Map
// ===========================
function updateGPS(gps) {
    if (gps.lat && gps.lng && gps.lat !== 0 && gps.lng !== 0) {
        // Update map marker
        const newLatLng = [gps.lat, gps.lng];
        marker.setLatLng(newLatLng);
        map.setView(newLatLng, 16);
        
        // Update path
        pathCoordinates.push(newLatLng);
        if (pathCoordinates.length > 100) {
            pathCoordinates.shift(); // Keep last 100 points
        }
        pathLine.setLatLngs(pathCoordinates);
        
        // Update coordinates display
        document.getElementById('gpsCoords').textContent = 
            `${gps.lat.toFixed(6)}, ${gps.lng.toFixed(6)}`;
    }
    
    // Update other GPS info
    document.getElementById('gpsAlt').textContent = gps.alt ? gps.alt.toFixed(1) + ' m' : '-- m';
    document.getElementById('gpsSpeed').textContent = gps.speed ? gps.speed.toFixed(1) + ' km/h' : '-- km/h';
    
    // Update satellite count
    const satCount = gps.satellites || 0;
    document.getElementById('satCount').textContent = satCount;
    
    const gpsQuality = document.getElementById('gpsQuality');
    if (satCount >= 6) {
        gpsQuality.style.background = 'rgba(16, 185, 129, 0.2)';
        gpsQuality.style.borderColor = '#10b981';
    } else if (satCount >= 4) {
        gpsQuality.style.background = 'rgba(245, 158, 11, 0.2)';
        gpsQuality.style.borderColor = '#f59e0b';
    } else {
        gpsQuality.style.background = 'rgba(239, 68, 68, 0.2)';
        gpsQuality.style.borderColor = '#ef4444';
    }
}

// ===========================
// Update IMU Data & Chart
// ===========================
function updateIMU(imu) {
    if (imu.accel_x !== undefined) {
        // Update chart data
        accelData.x.push(imu.accel_x);
        accelData.y.push(imu.accel_y);
        accelData.z.push(imu.accel_z);
        
        // Keep only last N points
        if (accelData.x.length > maxDataPoints) {
            accelData.x.shift();
            accelData.y.shift();
            accelData.z.shift();
        }
        
        // Update chart
        accelChart.update('none');
    }
    
    // Update temperature
    if (imu.temp !== undefined) {
        document.getElementById('imuTemp').textContent = imu.temp.toFixed(1) + '°C';
    }
}

// ===========================
// Update Gyro Data & Chart
// ===========================
function updateGyro(imu) {
    if (imu.gyro_x !== undefined) {
        // Update chart data
        gyroData.x.push(imu.gyro_x);
        gyroData.y.push(imu.gyro_y);
        gyroData.z.push(imu.gyro_z);
        
        // Keep only last N points
        if (gyroData.x.length > maxDataPoints) {
            gyroData.x.shift();
            gyroData.y.shift();
            gyroData.z.shift();
        }
        
        // Update chart
        gyroChart.update('none');
    }
}

// ===========================
// Update Navigation Data
// ===========================
function updateNavigation(nav) {
    const navCard = document.getElementById('navCard');
    
    if (nav && nav.distance !== undefined) {
        // Show navigation card
        navCard.style.display = 'block';
        
        // Distance
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
        
        // Active motor
        const motorIcons = ['⬆️', '➡️', '⬇️', '⬅️', '⏸️'];
        const motorNames = ['FRONT', 'RIGHT', 'BACK', 'LEFT', 'NONE'];
        const motorIndex = nav.active_motor || 0;
        
        document.getElementById('navMotorIcon').textContent = motorIcons[motorIndex];
        document.getElementById('navActiveMotor').textContent = motorNames[motorIndex];
        
        // Update compass
        drawCompass(nav.bearing, nav.heading);
        
        // Update badge
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
        navCard.style.display = 'none';
    }
}

// ===========================
// Draw Compass
// ===========================
function drawCompass(bearing, heading) {
    const canvas = document.getElementById('compassCanvas');
    const ctx = canvas.getContext('2d');
    const centerX = canvas.width / 2;
    const centerY = canvas.height / 2;
    const radius = 80;
    
    // Clear canvas
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    
    // Draw outer circle
    ctx.beginPath();
    ctx.arc(centerX, centerY, radius, 0, 2 * Math.PI);
    ctx.strokeStyle = '#334155';
    ctx.lineWidth = 2;
    ctx.stroke();
    
    // Draw heading indicator (user's direction)
    ctx.save();
    ctx.translate(centerX, centerY);
    ctx.rotate((heading - 90) * Math.PI / 180);
    ctx.beginPath();
    ctx.moveTo(0, -radius + 10);
    ctx.lineTo(-10, -radius + 25);
    ctx.lineTo(10, -radius + 25);
    ctx.closePath();
    ctx.fillStyle = '#06b6d4';
    ctx.fill();
    ctx.restore();
    
    // Draw bearing indicator (direction to target)
    ctx.save();
    ctx.translate(centerX, centerY);
    ctx.rotate((bearing - 90) * Math.PI / 180);
    ctx.beginPath();
    ctx.moveTo(0, -radius - 5);
    ctx.lineTo(-8, -radius + 15);
    ctx.lineTo(8, -radius + 15);
    ctx.closePath();
    ctx.fillStyle = '#8b5cf6';
    ctx.fill();
    ctx.restore();
    
    // Draw center dot
    ctx.beginPath();
    ctx.arc(centerX, centerY, 5, 0, 2 * Math.PI);
    ctx.fillStyle = '#f8fafc';
    ctx.fill();
    
    // Draw labels
    ctx.fillStyle = '#cbd5e1';
    ctx.font = '12px Inter';
    ctx.textAlign = 'center';
    ctx.fillText('N', centerX, centerY - radius - 10);
    ctx.fillText('S', centerX, centerY + radius + 20);
    ctx.fillText('E', centerX + radius + 15, centerY + 5);
    ctx.fillText('W', centerX - radius - 15, centerY + 5);
    
    // Draw legend
    ctx.textAlign = 'left';
    ctx.font = '10px Inter';
    ctx.fillStyle = '#06b6d4';
    ctx.fillText('■ Your Direction', 10, 15);
    ctx.fillStyle = '#8b5cf6';
    ctx.fillText('■ Target Direction', 10, 30);
}

// ===========================
// Reset Navigation
// ===========================
function resetNavigation() {
    pathCoordinates = [];
    pathLine.setLatLngs(pathCoordinates);
    packetCount = 0;
    console.log('Navigation reset');
}
