program Weatherstation_Project; // <-- NAVNET ER ÆNDRET HER FRA Weatherstation TIL Weatherstation_Project

{$mode objfpc}{$H+}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  {$IFDEF HASAMIGA}
  athreads,
  {$ENDIF}
  Interfaces,
  Forms,
  Unit1;

{$R *.res}

begin
  RequireDerivedFormResource := True;
  Application.Scaled := True;
  Application.Initialize;
  Application.CreateForm(TWeatherstation, Weatherstation); // Nu forstår compileren variablen korrekt!
  Application.Run;
end.

