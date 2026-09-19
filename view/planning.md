# Visualization-Tool overview

 The tool shall consist of a Python backend to receive the DAT's OSC messages, and serve a web-view for the data. The DAT can stream multiple format's of messages, but the backend will expect, receive and display the following messages:

- <prefix>/raw/all (all EEG channels)
- <prefix>/quality/all (contact quality for all EEG channels)
- <prefix>/gyro/x and <prefix>/gyro/y (gyroscope data)
- <prefix>/fft/<channel>
- <prefix>/battery

All these messages will timestamped with a timetag, i.e., the DAT will be started with `--osc-messages tbugyq --osc-timestamp timetag`.

The tool is started by executing an entry-point Python-script, e.g., `./xavier-viewer`. It loads or creates a `viewer.conf.json` (which stores the state and settings/parameters of the viewer and its widgets), and restore the viewer accordingly.

At a later point, the server will also emit OSC messages from analysis tools present in the viewer. We won't specify any such tools now, but keep that in mind.

## UI

The UI shall consist of a horizontal top menu bar and a main canvas on the rest of the screen. The menu bar allows general configuration and status overview: OSC port, prefix, connection status an battery level, as well as a play/pause button to interrupt/resume the data visualization at a viewer level; the menu also has toggeable buttons for opening widgets/panels in the main canvas. 

### Widgets

Widgets should open next to each other, in a column layout, taking up all vertical space. A divider between them should allow for horizontal resizing until a mininum width of 1/6 of the total screen width. Widgets' parameters should be concentrated within the widget's window, in a horizontal menu bar at its bottom. Ideally, this menu bar should have the same height accross all widgets, and should be minimizeable.

#### Data viewer

This is the primary widget that displays the `raw`, `quality` and `fft` messages. 

The bottom menu contains allows toggling visualization for all individual channels on and off (e.g., a toggle for AF3, F7, F3, etc.), and the user can choose between seeing all values displayed in raw values or approximate voltage (uV - conversion should happen with an approximate factor on the viewer itself). The user can also enable real-time raw data view and FFT view.

Each channel is displayed as a realtime graph; graphs are stacked vertically, and filling up the available vertical space. When real-time raw data view is enabled, data scrolls in realtime in each channel's graph. When FFT is enabled, each band's power is represented in a bar graph (bar's occupy their respective band's frequency range), sharing the same graph as the scrolling data - the scrolling data uses the left and bottom axis for value and time respectively, and the bar graph uses the right and top axis for value and frequency. Ensure the bars are perhaps slightly more visually faded and the scrolling graph uses a slightly more vibrant color to ensure contrast between the two.

#### Gyro viewer

This displays the `gyro` messages, and computes the heads' approximate yaw (x) and pitch (y) position. The widget displays at its top a visualization akin to a "level bubble" (which shows a data point for x/y and another for yaw/pitch), an below it, a scrolling data graph. In the menu, the user can enable viewing the x,y and/or yaw/pitch data. To estimate yaw/pitch, we'll use "high-passed integration": integrate each gyro axis' value, but apply a slight high-pass to it (configurable in the UI) to ensure that the estimated position always slowly drifts back to zero, and not to some infinite value.