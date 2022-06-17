import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import math

# time_limit = 20
start_fraction = 47/78
end_fraction = 48/77

data_csv = open("./CWND.csv", "r")
new_file = open("./Munged_CWND.csv", "w")

for row in data_csv:
    if len(row.strip().split(",")) == 5:
        new_file.write(row)

data_csv.close()
new_file.close()

df = pd.read_csv("./Munged_CWND.csv", header = None)

scaledTime = df[0] - df[0].min()
delta = scaledTime.max() - scaledTime.min()
lowerBound = start_fraction * delta
upperBound = end_fraction * delta

plt.plot(scaledTime, df[1], label = "Window Size")
plt.ylabel("CWND Size")
plt.xlabel("Time since Sending Start(s)")
plt.xlim([lowerBound,upperBound])
plt.title(f"Changes in CWND Size over {round(lowerBound, 0)}-to-{round(upperBound, 0)} s")
plt.grid(True, which="both")
plt.savefig('./CWNDPlot.pdf',dpi=1000)
