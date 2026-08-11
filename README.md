# 🚨 AI-Based Fall Detection System

An intelligent **AI-powered wearable fall detection system** developed using **ESP32**, **MPU6050**, **TinyML**, and **TensorFlow Lite** for real-time fall detection. The system continuously monitors human motion using accelerometer and gyroscope data, detects falls using a trained 1D Convolutional Neural Network (CNN), and sends emergency alerts with the user's location through IoT services.

---

# 🚀 Features

## 🤖 AI-Based Fall Detection

- Real-Time Motion Monitoring
- 1D Convolutional Neural Network (CNN)
- TinyML On-Device Inference
- TensorFlow Lite Model
- High-Accuracy Fall Classification
- False Alarm Reduction

---

## 📡 IoT Features

- Real-Time Sensor Data Acquisition
- GPS Location Tracking
- Emergency Alert Notifications
- OLED Display Status
- Buzzer Alert
- Wi-Fi Connectivity

---

## 📈 Motion Analysis

- Accelerometer Data Processing
- Gyroscope Data Processing
- Sliding Window Segmentation
- Data Normalization
- Motion Feature Extraction
- Continuous Monitoring

---

# 💻 Hardware Components

- ESP32 Development Board
- MPU6050 Accelerometer & Gyroscope
- NEO-6M GPS Module
- SSD1306 OLED Display
- Buzzer
- LED Indicator
- Power Supply

---

# 🧠 Machine Learning

- TensorFlow
- TensorFlow Lite
- TinyML
- 1D Convolutional Neural Network (CNN)
- SisFall Dataset
- Binary Classification (Fall / Non-Fall)

---

# 💻 Software Stack

## Embedded

- Arduino IDE
- ESP32 SDK
- C++

## AI / Machine Learning

- Python
- TensorFlow
- TensorFlow Lite
- NumPy
- Pandas
- Scikit-learn

## IoT

- Wi-Fi
- GPS
- Blynk
- TinyGPS++
- OLED Display Library

---

# 📂 Project Structure

```text
AI-Based-Fall-Detection-System
│
├── Arduino/
│   ├── ESP32_Code/
│   └── model.h
│
├── Model/
│   ├── Training.ipynb
│   ├── Dataset/
│   └── TFLite_Model/
│
├── Images/
├── Documentation/
├── README.md
```

---

# ✨ Key Features

- AI-Based Fall Detection
- TinyML Edge Inference
- TensorFlow Lite Deployment
- ESP32 Integration
- GPS Tracking
- IoT Alert System
- OLED Display
- Motion Monitoring
- Real-Time Processing
- Wearable Design

---

# 📊 Dataset

- SisFall Dataset
- Accelerometer Data
- Gyroscope Data
- Sliding Window Processing
- Binary Labels (Fall / ADL)

---

# ⚙️ Installation

## Clone Repository

```bash
git clone https://github.com/FlameDash25/AI-Based-Fall-Detection-System.git
```

## Machine Learning

```bash
pip install -r requirements.txt
```

Train the CNN model:

```bash
python train.py
```

Convert the trained model to TensorFlow Lite:

```bash
python convert_tflite.py
```

---

## ESP32

1. Open the Arduino project.
2. Install the required libraries.
3. Configure Wi-Fi credentials.
4. Upload the firmware to the ESP32.
5. Connect the MPU6050, GPS, OLED, and buzzer.
6. Monitor the output using the Serial Monitor.

---

# 📸 Screenshots

Add screenshots of:

- Hardware Prototype
- ESP32 Circuit
- OLED Display
- Serial Monitor
- Model Training
- Accuracy Graph
- Confusion Matrix
- Fall Detection Alerts

---

# 📈 Future Enhancements

- Mobile Application
- Cloud Dashboard
- Emergency Contact SMS
- Voice Assistance
- BLE Connectivity
- Multi-Fall Classification
- Health Monitoring
- Battery Optimization
- Firebase Integration
- Wearable Smartwatch Support

---

# 🎯 Learning Outcomes

This project strengthened practical knowledge of:

- TinyML
- TensorFlow Lite
- ESP32 Programming
- Embedded Systems
- Internet of Things (IoT)
- Deep Learning
- Convolutional Neural Networks (CNN)
- Sensor Data Processing
- GPS Integration
- Real-Time Machine Learning
- Git & GitHub

---

# 👨‍💻 Author

**Prasanna Kadrekar**

- GitHub: https://github.com/prxsanna

---

# 📄 License

This project is developed for educational and research purposes.
