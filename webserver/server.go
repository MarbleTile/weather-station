package main

import (
	"bytes"
	"fmt"
	"html/template"
	"io"
	"net/http"

	"github.com/labstack/echo/v4"
//	"github.com/labstack/echo/v4/middleware"
)

type Event struct {
    ID []byte
    Data []byte
    Event []byte
    Retry []byte
    Comment []byte
}

func (ev *Event) MarshalTo(w io.Writer) error {
	if len(ev.Data) == 0 && len(ev.Comment) == 0 {
		return nil
	}

	if len(ev.Data) > 0 {
		if _, err := fmt.Fprintf(w, "id: %s\n", ev.ID); err != nil {
			return err
		}

		sd := bytes.Split(ev.Data, []byte("\n"))
		for i := range sd {
			if _, err := fmt.Fprintf(w, "data: %s\n", sd[i]); err != nil {
				return err
			}
		}

		if len(ev.Event) > 0 {
			if _, err := fmt.Fprintf(w, "event: %s\n", ev.Event); err != nil {
				return err
			}
		}

		if len(ev.Retry) > 0 {
			if _, err := fmt.Fprintf(w, "retry: %s\n", ev.Retry); err != nil {
				return err
			}
		}
	}

	if len(ev.Comment) > 0 {
		if _, err := fmt.Fprintf(w, ": %s\n", ev.Comment); err != nil {
			return err
		}
	}

	if _, err := fmt.Fprint(w, "\n"); err != nil {
		return err
	}

	return nil
}

type tmpls struct {
    tmpl *template.Template
}

func (t *tmpls) Render(w io.Writer, name string, data interface{}, c echo.Context) error {
    return t.tmpl.ExecuteTemplate(w, name, data)
}

func init_tmpls() *tmpls {
    return &tmpls{
        tmpl: template.Must(template.ParseGlob("views/*.html")),
    }
}

func root(c echo.Context) error {
    return c.Render(http.StatusOK, "index", nil)
}

func sse(c echo.Context) error {
    w := c.Response()
    w.Header().Set("Content-Type", "text/event-stream")
    w.Header().Set("Cache-Control", "no-cache")
    w.Header().Set("Connection", "keep-alive")

    for {
        select {
        case <-c.Request().Context().Done():
            return nil
        case lt := <-localtemp_ch:
            ev := Event{
                Data: []byte("<h3>" + lt + "°C</h3>"),
                Event: []byte("localtemp"),
            }
            if err := ev.MarshalTo(w); err != nil {
                return err
            }
            w.Flush()
        case h := <-humi_ch:
            ev := Event{
                Data: []byte("<h3>" + h + "%</h3>"),
                Event: []byte("localhumi"),
            }
            if err := ev.MarshalTo(w); err != nil {
                return err
            }
            w.Flush()
        case ot := <-outtemp_ch:
            ev := Event{
                Data: []byte("<h3>" + ot + "</h3>"),
                Event: []byte("outtemp"),
            }
            if err := ev.MarshalTo(w); err != nil {
                return err
            }
            w.Flush()
        }
    }
}

func weather(c echo.Context) error {
    lt := c.FormValue("localtemp")
    if lt != "" {
        localtemp_ch <- lt
        fmt.Printf("POSTed local temp: %s°C\n", lt)
    }
    h := c.FormValue("localhumi")
    if h != "" {
        humi_ch <- h
        fmt.Printf("POSTed local humi: %s%%\n", h)
    }
    ot := c.FormValue("outtemp")
    if ot != "" {
        outtemp_ch <- ot
        fmt.Printf("POSTed outer temp: %s\n", ot)
    }
    fmt.Println("---")
    return nil
}

func location(c echo.Context) error {
    return c.String(http.StatusOK, "Santa+Cruz")
}

var localtemp_ch = make(chan string, 1)
var humi_ch = make(chan string, 1)
var outtemp_ch = make(chan string, 1)
func main() {

    e := echo.New()
//    e.Use(middleware.LoggerWithConfig(middleware.LoggerConfig{
//        Format: "${method} ${uri} ${status}\n",
//    }))

    e.Renderer = init_tmpls()

    e.GET("/", root)
    e.GET("/sse", sse)
    e.GET("/location", location)

    e.POST("/weather", weather)

    e.Static("/static", "static")

    e.Logger.Fatal(e.Start(":1234"))
}

