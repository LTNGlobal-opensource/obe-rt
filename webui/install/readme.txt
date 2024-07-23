
unpack the tarball into /home/ltn_encoder

cd ui

drwxrwxr-x. 3 stoth stoth    41 Jul  2 11:57 html
drwxrwxr-x. 2 stoth stoth    77 Jul 23 12:54 install
-rw-rw-r--. 1 stoth stoth    47 Jul  2 11:57 package.json
-rw-rw-r--. 1 stoth stoth 11836 Jul 23 12:38 server.js

# Install any node modules
npm install

# Install the systemd configuration
cp install/ltn-encoder-ui.service /usr/lib/systemd/system/ltn-encoder-ui.service
systemctl enable ltn-encoder-ui
systemctl start ltn-encoder-ui

# Deal with the firewall, the port list is 
13300 http server
13400 websockets
