# **Emotiv EPOC Development Guide: Emotion & Cognitive State Analysis**

# **1\. Overview**

This guide provides the necessary mathematical heuristics and channel mappings to implement real-time emotion and cognitive state detection using the 14-channel Emotiv EPOC headset. This approach bypasses machine learning in favor of robust, direct band-power calculations, making it ideal for zero-training interactive art installations.

# **2\. Prerequisites**

* **Hardware:** Emotiv EPOC (14 channels: AF3, F7, F3, FC5, FC6, F4, F8, AF4, P7, P8, T7, T8, O1, O2).  
* **Data Stream:** Real-time access to computed power bands (Delta, Theta, Alpha, Beta, Gamma) for each individual channel.

# **3\. The 2D Emotion Model (Valence & Arousal)**

This maps the user's state onto Russell’s Circumplex Model using two independent axes.

## **Valence (Positive vs. Negative Mood)**

Based on Frontal Alpha Asymmetry (left brain \= approach/positive; right brain \= withdrawal/negative). Because Alpha is inversely correlated with brain activity, we compare the inverse power.

* **Primary Channels:** F3 (Left) and F4 (Right). (Can be averaged with AF3/AF4 for stability).  
* **Formula:** `Valence = ln(Alpha_F4) - ln(Alpha_F3)`  
* **Interpretation:** Positive result \= Positive mood/Happy. Negative result \= Negative mood/Sad/Anxious.

## **Arousal (Excitement / Engagement)**

Measures alertness based on the ratio of fast-wave to slow-wave activity.

* **Primary Channels:** Average across frontal channels.  
* **Formula:** `Arousal = Beta / Alpha`  
* **Interpretation:** High ratio \= Excited/Stimulated/Stressed. Low ratio \= Calm/Bored.

## **2D State Mapping**

Combine the two indices to drive installation logic (e.g., soundscapes, colors):

* **High Arousal \+ Positive Valence:** Excited, Joyful  
* **High Arousal \+ Negative Valence:** Stressed, Angry  
* **Low Arousal \+ Positive Valence:** Calm, Relaxed  
* **Low Arousal \+ Negative Valence:** Bored, Depressed

# **4\. Cognitive State Analysis**

For installations focusing on mental workload or meditation.

## **Focus / Cognitive Load**

* **Primary Channels:** F3, F4, F7, F8  
* **Formula:** `Focus = Beta / (Alpha + Theta)`  
* **Interpretation:** Increases when the user concentrates hard on a task.

## **Deep Relaxation (The "Eye-Close" Trigger)**

* **Primary Channels:** O1, O2 (Occipital lobe)  
* **Trigger:** A massive, sudden spike in absolute Alpha power.  
* **Interpretation:** The user has closed their eyes and relaxed their visual cortex. Highly reliable as a binary switch.

# **5\. Development Best Practices**

* **Baseline Normalization:** Absolute band power varies significantly between skulls. Implement a trailing 30-second rolling average, or require a 10-second "calibration" phase on startup to establish the user's baseline (`Valence = 0`, `Arousal = 0`).  
* **Signal Smoothing:** Raw EEG triggers flutter rapidly. Pass all final indices (Valence, Arousal, Focus) through a moving average (e.g., averaging the last 1-2 seconds of calculated output) or a low-pass filter to ensure smooth transitions in your application.  
* **Embrace EMG Artifacts:** Do not aggressively filter facial muscles. Blinks (massive Delta spikes on AF3/AF4) and jaw clenches (Gamma/Beta spikes on F7/F8) correlate heavily with user tension and intention. Let them bleed into your indices to make the installation feel highly responsive.

