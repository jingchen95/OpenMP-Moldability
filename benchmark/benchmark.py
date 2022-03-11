import subprocess
import pandas as pd
import numpy as np
from sklearn.cluster import KMeans
import os.path

tests={'./memcpy', "./matmul", "./dotprod", "./jacobi", "simple-vgg16/vgg16"} 

data = []

if os.path.isfile('AI_data.txt'):
	
	with open('AI_data.txt', 'r') as filehandle:
    		for line in filehandle:
        		# remove linebreak which is the last character of the string
        		currentPlace = line[:-1]

        		# add item to the list
        		data.append(int (currentPlace))

else: 
	for test in tests:
		print("Start benchmarking" + test)
		for i in range(0,10):
			process = subprocess.run(test,shell=True, capture_output=True, text=True)
			data.append(int (process.stdout))
			
		print("Done benchmarking " + test)
		
	with open('AI_data.txt', 'w') as filehandle:
    		for listitem in data:
        		filehandle.write('%s\n' % listitem)

for entry in data:
	if entry > 50000:
		data.remove(entry)
			
		
df = pd.DataFrame({
    'AI': data,
})


print(df)

kmeans = KMeans(n_clusters=3)
kmeans.fit(df)
centers = []
for entry in kmeans.cluster_centers_:
	centers.append(entry[0])

centers.sort()

print(centers)

low = centers[0] + (centers[1] - centers[0])/2

high = centers[1] + (centers[2] - centers[1])/2

print("Low limit : %2d, high limit : %2d" % (int (low), int(high)))
