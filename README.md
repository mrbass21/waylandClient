# Handmade Wayland Client

The intention with this codebase is to be a reference or starting point for making a very basic Wayland client. 

The motivation for this comes from Handmade Hero and making this an initial starting point for this client to be used with the game.

Features:
 - [x] Draw a window with client side decorations
 - [x] Accept keyboard input
 - [x] Accept controller input
 - [ ] Write sound

 My intention is to get all the basic client operations filled in as Handmade Hero uses. The main branch will contain the "final" client code to take and just jump into doing the game code.

 I also intend to create branches by day that will attempt to implent the minimum of what we would need in wayland to be in step with the Handmade Hero series. Note that I plan on getting the basics inside this client and then going back to the day branches, so they will come last.

 ## Known Limitations
 ### Client Side Decorations
 Client side decorations are the close, minimize, maximize and title bar for the window. The Wayland protocol has taken a position that it is expected for the client to draw thier own decorations (meaning by default it will just be a window of your buffer, no close button or bar to move the window around).

 However, there is an experemential protocol for asking the wayland server (your window manager) to draw those client side decorations for you. This is how Windows and macOS operate by default.

 I use KDE, which does implement that protocol.

 If your window manager doesn't support drawing server side decorations, you won't see those items.

 Your options are to either install and run a window manager that does implement that protocol, or to add your own client side decorations to this window. I won't be adding that, but it is possible to do.

 ### Controller dectection
 Controller detection happens through udev, which should be portable to all Linux OSes, however I'm sure there's some distribtion that will make a liar of me.

 There is some code to filter udev devices by subsystem 'input' and then further applies some filtering rules. If your controller is not detected, you might need to tweak that filtering so that your device is found. 

## References
I used https://developer.orbitrc.io/documentation/wayland/guides/hello-wayland/ as a resource, but it appears to not exist anymore.

The [Wayland Book](https://wayland-book.com/introduction.html) is a great resource as well.

I used AI to help with the controller input as well as restructure to be frame based instead of event based.