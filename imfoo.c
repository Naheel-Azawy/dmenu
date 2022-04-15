/* include X11 stuff */
#include <X11/Xlib.h>
/* include Imlib2 stuff */
#include <Imlib2.h>
/* sprintf include */
#include <stdio.h>
#include <stdlib.h>

/* some globals for our window & X display */
Display *disp;
Window   win;
Visual  *vis;
Colormap cm;
int      depth;

/* load and scale icon */
static Imlib_Image
drw_icon(const char *file, int x, int y)
{
	Imlib_Image icon;
	int width;
	int height;
	int imgsize;

	icon = imlib_load_image(file);

	if (icon == NULL)
		return NULL;

	imlib_context_set_image(icon);

	width = imlib_image_get_width();
	height = imlib_image_get_height();
	imgsize = width > height ? height : width;

	icon = imlib_create_cropped_scaled_image(0, 0, imgsize, imgsize,
											 64, 64);

	imlib_context_set_image(icon);
	imlib_render_image_on_drawable(x, y);
	imlib_free_image();

	return icon;
}

/* the program... */
int main(int argc, char **argv)
{
   /* events we get from X */
   XEvent ev;
   /* areas to update */
   Imlib_Updates updates, current_update;

   /* connect to X */
   disp  = XOpenDisplay(NULL);
   /* get default visual , colormap etc. you could ask imlib2 for what it */
   /* thinks is the best, but this example is intended to be simple */
   vis   = DefaultVisual(disp, DefaultScreen(disp));
   depth = DefaultDepth(disp, DefaultScreen(disp));
   cm    = DefaultColormap(disp, DefaultScreen(disp));
   /* create a window 640x480 */
   win = XCreateSimpleWindow(disp, DefaultRootWindow(disp), 
                             0, 0, 640, 480, 0, 0, 0);
   /* tell X what events we are interested in */
   XSelectInput(disp, win, ButtonPressMask | ButtonReleaseMask | 
                PointerMotionMask | ExposureMask);
   /* show the window */
   XMapWindow(disp, win);
   /* set our cache to 2 Mb so it doesn't have to go hit the disk as long as */
   /* the images we use use less than 2Mb of RAM (that is uncompressed) */
   imlib_set_cache_size(2048 * 1024);
   /* set the maximum number of colors to allocate for 8bpp and less to 128 */
   imlib_set_color_usage(128);
   /* dither for depths < 24bpp */
   imlib_context_set_dither(1);
   /* set the display , visual, colormap and drawable we are using */
   imlib_context_set_display(disp);
   imlib_context_set_visual(vis);
   imlib_context_set_colormap(cm);
   //imlib_context_set_drawable(win);
   /* infinite event loop */
   for (;;)
     {
        /* image variable */
        Imlib_Image image;
        
        /* init our updates to empty */
        updates = imlib_updates_init();
        /* while there are events form X - handle them */
        do
          {
             XNextEvent(disp, &ev);
             switch (ev.type)
               {
               case Expose:
                  /* window rectangle was exposed - add it to the list of */
                  /* rectangles we need to re-render */
                  updates = imlib_update_append_rect(updates,
                                                     ev.xexpose.x, ev.xexpose.y,
                                                     ev.xexpose.width, ev.xexpose.height);
                  break;
               default:
                  /* any other events - do nothing */
                  break;
               }
          }
        while (XPending(disp));
        
        /* no more events for now ? ok - idle time so lets draw stuff */
        
        /* take all the little rectangles to redraw and merge them into */
        /* something sane for rendering */
        updates = imlib_updates_merge_for_rendering(updates, 640, 480);
        for (current_update = updates; 
             current_update; 
             current_update = imlib_updates_get_next(current_update))
          {
            printf("aaaaaaaaaaaaaaaaaaa\n");
            image = drw_icon("/mnt/hdd1/Private/Pictures/Windows/bliss/bliss.jpg", 0, 0);
          }
        /* if we had updates - free them */
        if (updates)
           imlib_updates_free(updates);
        /* loop again waiting for events */
     }
   return 0;
}
