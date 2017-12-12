# File-Server    
A multithreaded file server + client to execute GET and PUT requests. Includes MD5 Encryption  

#### Quick start  
From within the project folder run the server with `./obj64/Server -p <PORT> <ARGS>`  
````
-m    enable multithreading mode  
-l    number of entries in the LRU cache  
-p    port on which to listen for connections . 
````
From another folder make GET and PUT requests to the server with the client `./<PATH>/File-Server/obj64/Client -s <SERVER> -p <PORT>`  
````
-P <filename>   PUT file indicated by parameter  
-G <filename>   GET file indicated by parameter  
-s              server info (IP or hostname)  
-p              port on which to contact server  
-S <filename>   for GETs, name to use when saving file locally  
-c              enable MD5 encryption   
````
###### Compiled with gcc-7.1.0 . 
`make clean && make`
