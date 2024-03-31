# EMI USB sound card

### The problem

<p>The EMI6|2 sound card a.k.a. Emagic A62m is having issues in linux.<br>
  
[Here you can read more about it, but the downloads are not working anymore](https://xiphmont.dreamwidth.org/46812.html "Original page")

</p>


### The solution

<p>Download this repo to a local directory<br>
Make sure to have installed the same compiler as what your OS was build with.<br>
Ubuntu 22 LTS is using gcc-12<br>

Go to the directory: emi_xipmont_source<br>
Type: `make`<br>
When succesful, type: `make install`<br>
</p>

### Linux kernel update

<p>Beware that when the linux kernel is updated, you might need to run the above steps again</p>

## Enjoy your sound card!

