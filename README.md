# web_crawler_proj
Group Project for 3 people

1. One person must write a function in C++ with networking code that will pull a website from the interwebs, using readlib (https://github.com/ShakaUVM/readLinks to an external site.) and tcpstreams (https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/ref/boost__beast__tcp_stream.htmlLinks to an external site.) and return a vector with a list of strings containing the URLs found in that website

Demo code: /public/network_tcpstreams.cc

2. Another person builds a graph out of the connections from the website and call the function for #1 for any site not visited yet (i.e. do graph traversal). The max depth from the starting URL is configurable as is the maximum size of the graph. I would start with a max depth of three and a max graph size of a thousand. Add a .1s sleep (use usleep) between requests so you don't get my IP address banned. Implement both BFS and DFS.

3. Visualize the graph on BRIDGES. Each node should be colored based on how many hops away from the origin it is, with a label when you mouse over it for the URL that it is. And also maybe the HTML name or something. The root node (distance 0) should be red, distance 1 (one hop away from the start) should be orange, distance 2 should be yellow, distance 3 should be G, following ROYGBIV in other words. Show all links, and make the lines bold if they link back or something. Links should be directional with arrows. 

Extra Credit will be offered for: Finishing BRIDGES and not ignoring it like you guys always do, j/k do something cooler with BRIDGES than the minimum requirements.

Visualize this for multiple websites and put the visualizations into a report

Wikipedia War of the Roses

Google search

ESPN

Wowcubedocs.com

Dwarfdig.com
