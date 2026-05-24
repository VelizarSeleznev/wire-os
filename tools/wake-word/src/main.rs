use rustpotter::{Rustpotter, RustpotterConfig};
use std::net::UdpSocket;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

fn main() {
    println!("Starting Vector Wake Word Service...");

    // 1. Initialize Rustpotter detector with default config
    let config = RustpotterConfig::default();
    let mut potter = Rustpotter::new(&config).expect("failed to create Rustpotter detector");

    // 2. Load any .rpw or .wakeword files in current directory
    let mut loaded = false;
    if let Ok(entries) = std::fs::read_dir(".") {
        for entry in entries.flatten() {
            let path = entry.path();
            if let Some(ext) = path.extension() {
                if ext == "rpw" || ext == "wakeword" {
                    println!("Loading wakeword model: {:?}", path);
                    let path_str = path.to_str().unwrap_or("");
                    if let Err(e) = potter.add_wakeword_from_file("hey_vector", path_str) {
                        eprintln!("Failed to load wakeword model {:?}: {:?}", path, e);
                    } else {
                        println!("Successfully loaded model {:?}", path);
                        loaded = true;
                    }
                }
            }
        }
    }

    if !loaded {
        eprintln!("WARNING: No wake word models (.rpw or .wakeword) loaded. Please place a model file in this folder.");
    }

    // 3. Keep trying to start the UDP audio stream from the robot API
    let stop_stream_request = Arc::new(AtomicBool::new(false));
    let stop_signal = stop_stream_request.clone();
    thread::spawn(move || {
        let client = ureq::Agent::new();
        while !stop_signal.load(Ordering::Relaxed) {
            println!("Requesting audio stream from vector-hw-api...");
            let res = client.post("http://127.0.0.1:8080/v1/audio/stream/start")
                .set("Content-Type", "application/json")
                .send_string("{\"ip\":\"127.0.0.1\",\"port\":5005}");
            
            match res {
                Ok(_) => {
                    println!("Successfully registered audio stream at 127.0.0.1:5005");
                    // Sleep longer once registered, checking status or re-requesting if stream dies
                    thread::sleep(Duration::from_secs(15));
                }
                Err(e) => {
                    eprintln!("Failed to request audio stream: {:?}. Retrying in 3 seconds...", e);
                    thread::sleep(Duration::from_secs(3));
                }
            }
        }
    });

    // 4. Bind UDP socket to listen for the stream
    let socket = UdpSocket::bind("127.0.0.1:5005").expect("failed to bind UDP socket to port 5005");
    println!("UDP audio receiver bound to 127.0.0.1:5005");

    let frame_size = potter.get_samples_per_frame();
    println!("Rustpotter expects frame size: {} samples", frame_size);

    let mut audio_buffer = Vec::new();
    let mut packet_buf = [0u8; 1024];

    // Exponential Moving Average smoothed energy for the 4 channels
    let mut smoothed_energy = [0.0f64; 4];
    let alpha = 0.1; // Smoothing factor
    let mut current_best_channel = 0;
    let mut channel_switch_counter = 0;

    while !stop_stream_request.load(Ordering::Relaxed) {
        match socket.recv_from(&mut packet_buf) {
            Ok((amt, _src)) => {
                // Header (20 bytes) + audio payload (320 samples * 2 bytes = 640 bytes) = 660 bytes
                if amt < 20 {
                    continue;
                }

                // Check magic bytes (stored as little-endian 0x56415544 "VAUD")
                let magic = u32::from_le_bytes([packet_buf[0], packet_buf[1], packet_buf[2], packet_buf[3]]);
                if magic != 0x56415544 {
                    continue;
                }

                let payload_len = u32::from_le_bytes([packet_buf[16], packet_buf[17], packet_buf[18], packet_buf[19]]) as usize;
                if amt < 20 + payload_len {
                    continue;
                }

                let raw_payload = &packet_buf[20..20 + payload_len];
                let num_samples = payload_len / 2;
                if num_samples < 4 {
                    continue;
                }

                // Convert bytes to i16 samples
                let mut samples = vec![0i16; num_samples];
                for i in 0..num_samples {
                    samples[i] = i16::from_le_bytes([raw_payload[2 * i], raw_payload[2 * i + 1]]);
                }

                // Audio is 4 channels interleaved: ch0, ch1, ch2, ch3, ch0, ch1, ch2, ch3...
                let samples_per_channel = num_samples / 4;
                if samples_per_channel == 0 {
                    continue;
                }

                // Calculate energy for each of the 4 channels
                let mut channel_energies = [0.0f64; 4];
                for ch in 0..4 {
                    let mut sum_sq = 0.0f64;
                    for step in 0..samples_per_channel {
                        let val = samples[step * 4 + ch] as f64;
                        sum_sq += val * val;
                    }
                    channel_energies[ch] = sum_sq / (samples_per_channel as f64);
                }

                // Update EMA smoothed energies
                for ch in 0..4 {
                    smoothed_energy[ch] = alpha * channel_energies[ch] + (1.0 - alpha) * smoothed_energy[ch];
                }

                // Find channel with the highest smoothed energy (best audio stream selection)
                let mut best_ch = 0;
                let mut max_energy = smoothed_energy[0];
                for ch in 1..4 {
                    if smoothed_energy[ch] > max_energy {
                        max_energy = smoothed_energy[ch];
                        best_ch = ch;
                    }
                }

                // Simple hysteresis to prevent too rapid channel switching
                if best_ch != current_best_channel {
                    channel_switch_counter += 1;
                    if channel_switch_counter >= 15 { // Require ~75ms of consistent higher energy to switch
                        println!("Switching audio channel: {} -> {} (energy: {:.0} vs {:.0})", 
                                 current_best_channel, best_ch, max_energy, smoothed_energy[current_best_channel]);
                        current_best_channel = best_ch;
                        channel_switch_counter = 0;
                    }
                } else {
                    channel_switch_counter = 0;
                }

                // Extract and convert the selected channel's samples to mono f32 (-1.0 to 1.0)
                let mut mono_samples_f32 = vec![0.0f32; samples_per_channel];
                for step in 0..samples_per_channel {
                    mono_samples_f32[step] = samples[step * 4 + current_best_channel] as f32 / 32768.0;
                }

                // Accumulate f32 samples in our detector buffer
                audio_buffer.extend_from_slice(&mono_samples_f32);

                // If we have enough samples for a Rustpotter frame, process it
                while audio_buffer.len() >= frame_size {
                    let frame: Vec<f32> = audio_buffer.drain(0..frame_size).collect();
                    
                    // Zero-copy cast f32 slice to u8 byte slice for process_bytes
                    let bytes: &[u8] = unsafe {
                        std::slice::from_raw_parts(
                            frame.as_ptr() as *const u8,
                            frame.len() * 4
                        )
                    };
                    
                    if let Some(detection) = potter.process_bytes(&bytes) {
                        println!(
                            "{{\"type\":\"detection\",\"wakeword\":\"{}\",\"score\":{},\"channel\":{}}}", 
                            detection.name, detection.score, current_best_channel
                        );

                        // Trigger backpack LEDs flash cyan in non-blocking background thread
                        thread::spawn(|| {
                            let client = ureq::Agent::new();
                            let cyan_body = r#"[{"r":0,"g":240,"b":255},{"r":0,"g":240,"b":255},{"r":0,"g":240,"b":255},{"r":0,"g":240,"b":255}]"#;
                            if let Err(e) = client.post("http://127.0.0.1:8080/v1/leds/backpack")
                                .set("Content-Type", "application/json")
                                .send_string(cyan_body) 
                            {
                                eprintln!("Failed to flash LEDs: {:?}", e);
                            }

                            thread::sleep(Duration::from_millis(1000));

                            let off_body = r#"[{"r":0,"g":0,"b":0},{"r":0,"g":0,"b":0},{"r":0,"g":0,"b":0},{"r":0,"g":0,"b":0}]"#;
                            let _ = client.post("http://127.0.0.1:8080/v1/leds/backpack")
                                .set("Content-Type", "application/json")
                                .send_string(off_body);
                        });
                    }
                }
            }
            Err(e) => {
                eprintln!("Socket receive error: {:?}", e);
                thread::sleep(Duration::from_millis(100));
            }
        }
    }
}
