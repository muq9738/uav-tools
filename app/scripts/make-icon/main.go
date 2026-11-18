// make-icon 把渲染好的方形图标裁成 macOS 风格圆角并写入透明通道。
//
// 浏览器截图只能给出不透明位图；这里按同一圆角矩形计算覆盖率，
// 用 3×3 超采样得到边缘的 alpha，避免出现白角。
package main

import (
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
)

func main() {
	input := flag.String("in", "", "输入 PNG")
	output := flag.String("out", "", "输出 PNG（带透明圆角）")
	inset := flag.Float64("inset", 64, "圆角矩形内缩像素")
	radius := flag.Float64("radius", 200, "圆角半径像素")
	flag.Parse()
	if *input == "" || *output == "" {
		fmt.Fprintln(os.Stderr, "用法：make-icon -in icon-square.png -out icon.png [-inset 64] [-radius 200]")
		os.Exit(2)
	}
	source, err := readPNG(*input)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	result := rounded(source, *inset, *radius)
	if err := writePNG(*output, result); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("已写入 %s（%dx%d，含透明圆角）\n", *output, result.Bounds().Dx(), result.Bounds().Dy())
}

func readPNG(path string) (image.Image, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	source, err := png.Decode(file)
	if err != nil {
		return nil, fmt.Errorf("解码 %s：%w", path, err)
	}
	return source, nil
}

func writePNG(path string, source image.Image) error {
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	defer file.Close()
	return png.Encode(file, source)
}

// rounded 只保留圆角矩形内的内容；边缘用 3×3 采样得到部分透明。
func rounded(source image.Image, inset, radius float64) *image.NRGBA {
	bounds := source.Bounds()
	target := image.NewNRGBA(bounds)
	left, top := float64(bounds.Min.X)+inset, float64(bounds.Min.Y)+inset
	right, bottom := float64(bounds.Max.X)-inset, float64(bounds.Max.Y)-inset
	const samples = 3
	for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
		for x := bounds.Min.X; x < bounds.Max.X; x++ {
			covered := 0
			for sy := 0; sy < samples; sy++ {
				for sx := 0; sx < samples; sx++ {
					px := float64(x) + (float64(sx)+0.5)/samples
					py := float64(y) + (float64(sy)+0.5)/samples
					if insideRounded(px, py, left, top, right, bottom, radius) {
						covered++
					}
				}
			}
			alpha := uint8(math.Round(float64(covered) / (samples * samples) * 255))
			r, g, b, _ := source.At(x, y).RGBA()
			target.Set(x, y, color.NRGBA{R: uint8(r >> 8), G: uint8(g >> 8), B: uint8(b >> 8), A: alpha})
		}
	}
	return target
}

// insideRounded 判断点是否落在圆角矩形内（角落按圆弧处理）。
func insideRounded(x, y, left, top, right, bottom, radius float64) bool {
	if x < left || x > right || y < top || y > bottom {
		return false
	}
	cx := math.Min(math.Max(x, left+radius), right-radius)
	cy := math.Min(math.Max(y, top+radius), bottom-radius)
	dx, dy := x-cx, y-cy
	return dx*dx+dy*dy <= radius*radius
}
