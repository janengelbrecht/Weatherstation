unit Unit1;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, Forms, Controls, Graphics, Dialogs, StdCtrls, serial
  {$IFDEF WINDOWS}
  , registry
  {$ENDIF}
  ;

type

  { TWeatherstation }

  TWeatherstation = class(TForm)
    Button1: TButton;
    ComboBox1: TComboBox;
    Edit1: TEdit; // Land
    Edit2: TEdit; // By
    Edit3: TEdit; // Longitude
    Edit4: TEdit; // Latitude
    Geografi: TGroupBox;
    By: TLabel;
    Label1: TLabel;
    Lat: TLabel;
    Long: TLabel;
    Land: TLabel;
    SSIDInput: TEdit;
    PasswordInput: TEdit;
    Wifi: TGroupBox;
    SSID: TLabel;
    Password: TLabel;
    procedure Button1Click(Sender: TObject);
    procedure FormCreate(Sender: TObject);
    procedure PasswordInputChange(Sender: TObject);
    procedure SSIDClick(Sender: TObject);
  private
    function PakStreng(const Input: string; Laengde: Integer): string;
    function ValiderInput(): Boolean;
  public

  end;

var
  Weatherstation: TWeatherstation;

implementation

{$R *.lfm}

{ TWeatherstation }

function TWeatherstation.PakStreng(const Input: string; Laengde: Integer): string;
begin
  Result := Input;
  if Length(Result) < Laengde then
    Result := Result + StringOfChar(' ', Laengde - Length(Result));
end;

function TWeatherstation.ValiderInput(): Boolean;
begin
  Result := False;

  if Trim(SSIDInput.Text) = '' then
  begin
    ShowMessage('Fejl: SSID må ikke være tomt.');
    SSIDInput.SetFocus;
    Exit;
  end;

  if Trim(Edit2.Text) = '' then
  begin
    ShowMessage('Fejl: Bynavn må ikke være tomt.');
    Edit2.SetFocus;
    Exit;
  end;

  if Length(SSIDInput.Text) > 50 then
  begin
    ShowMessage('Fejl: SSID er for langt (max 50 tegn).');
    SSIDInput.SetFocus;
    Exit;
  end;

  if Length(PasswordInput.Text) > 50 then
  begin
    ShowMessage('Fejl: Password er for langt (max 50 tegn).');
    PasswordInput.SetFocus;
    Exit;
  end;

  if Length(Edit1.Text) > 20 then
  begin
    ShowMessage('Fejl: Land er for langt (max 20 tegn).');
    Edit1.SetFocus;
    Exit;
  end;

  if Length(Edit2.Text) > 40 then
  begin
    ShowMessage('Fejl: By er for lang (max 40 tegn).');
    Edit2.SetFocus;
    Exit;
  end;

  if Length(Edit3.Text) > 7 then
  begin
    ShowMessage('Fejl: Longitude (Længdegrad) er for lang (max 7 tegn).' + sLineBreak +
                'ESP8266 protokollen tillader pt. kun 7 tegn.' + sLineBreak +
                'Fjern venligst decimaler, så det passer (f.eks. 10.2039).');
    Edit3.SetFocus;
    Exit;
  end;

  if Length(Edit4.Text) > 7 then
  begin
    ShowMessage('Fejl: Latitude (Breddegrad) er for lang (max 7 tegn).' + sLineBreak +
                'ESP8266 protokollen tillader pt. kun 7 tegn.' + sLineBreak +
                'Fjern venligst decimaler, så det passer (f.eks. 56.162).');
    Edit4.SetFocus;
    Exit;
  end;

  Result := True;
end;

procedure TWeatherstation.Button1Click(Sender: TObject);
var
  SerialHandle: TSerialHandle;
  PortNavn: string;
  SamletDataPakke: string;
  Buffer: array[0..255] of Char;
  BytesLaest: LongInt;
  ModtagetAkkumuleret: string = '';
  KlarFundet: Boolean = false;

  StartTid: QWord;
  TimeoutMillisekunder: QWord;
begin
  PortNavn := Trim(ComboBox1.Text);
  if (PortNavn = '') or (PortNavn = 'Ingen COM-porte fundet') then
  begin
    ShowMessage('Vælg venligst en gyldig seriel port i listen.');
    Exit;
  end;

  if not ValiderInput() then
    Exit;

  SamletDataPakke := PakStreng(SSIDInput.Text, 50) +
                     PakStreng(PasswordInput.Text, 50) +
                     PakStreng(Edit1.Text, 20) +
                     PakStreng(Edit2.Text, 40) +
                     PakStreng(Edit3.Text, 7) +
                     PakStreng(Edit4.Text, 7);

  SerialHandle := SerOpen(PortNavn);
  if SerialHandle < 0 then
  begin
    ShowMessage('Fejl: Kunne ikke åbne seriel port: ' + PortNavn + sLineBreak +
                'Tjek om porten er i brug af et andet program (f.eks. Arduino Monitor).');
    Exit;
  end;

  Screen.Cursor := crHourGlass;

  try
    try
      SerSetParams(SerialHandle, 9600, 8, EvenParity, 1, []);

      TimeoutMillisekunder := 5000;
      StartTid := GetTickCount64;

      while (not KlarFundet) and ((GetTickCount64 - StartTid) < TimeoutMillisekunder) do
      begin
        FillChar(Buffer, SizeOf(Buffer), 0);

        BytesLaest := SerRead(SerialHandle, Buffer, SizeOf(Buffer) - 1);
        if BytesLaest > 0 then
        begin
          Buffer[BytesLaest] := #0;
          ModtagetAkkumuleret := ModtagetAkkumuleret + StrPas(Buffer);

          if Pos('klar', LowerCase(ModtagetAkkumuleret)) > 0 then
            KlarFundet := true;
        end;

        Sleep(20);
        Application.ProcessMessages;
      end;

      if KlarFundet then
      begin
        SerWrite(SerialHandle, SamletDataPakke, Length(SamletDataPakke));
        Sleep(500);
        ShowMessage('Data sendt til ESP8266. Vejrstationen bør nu genstarte.');
      end
      else
      begin
        ShowMessage('Fejl: Forbindelsen fik timeout!' + sLineBreak + sLineBreak +
                    'ESP8266 svarede ikke med "klar" inden for 5 sekunder.' + sLineBreak +
                    'Tjek venligst følgende:' + sLineBreak +
                    '1. Er ESP8266 tændt og genstartet?' + sLineBreak +
                    '2. Sender den ordet "klar" via seriel port?' + sLineBreak +
                    '3. Er baudrate (9600) og paritet (Even) sat rigtigt på ESP8266?');
      end;

    except
      on E: Exception do
        ShowMessage('Der opstod en uventet fejl under kommunikationen: ' + E.Message);
    end;

  finally
    SerClose(SerialHandle);
    Screen.Cursor := crDefault;
  end;
end;


procedure TWeatherstation.PasswordInputChange(Sender: TObject);
begin
end;

procedure TWeatherstation.FormCreate(Sender: TObject);
{$IFDEF WINDOWS}
var
  Reg: TRegistry;
  Names: TStringList;
  i: Integer;
{$ELSE}
var
  SR: TSearchRec;
{$ENDIF}
begin
  ComboBox1.Items.Clear;

  {$IFDEF WINDOWS}
  // På Windows trækker vi portene dynamisk fra registreringsdatabasen (ingen admin-rettigheder krævet via ReadOnly)
  Reg := TRegistry.Create;
  Names := TStringList.Create;
  try
    Reg.RootKey := HKEY_LOCAL_MACHINE;
    if Reg.OpenKeyReadOnly('\HARDWARE\DEVICEMAP\SERIALCOMM') then
    begin
      Reg.GetValueNames(Names);
      for i := 0 to Names.Count - 1 do
        ComboBox1.Items.Add(Reg.ReadString(Names[i]));
    end;
  finally
    Names.Free;
    Reg.Free;
  end;
  {$ELSE}
  // På Linux/macOS skanner vi standardenheder i /dev/
  if FindFirst('/dev/ttyUSB*', faAnyFile, SR) = 0 then
  begin
    repeat
      ComboBox1.Items.Add('/dev/' + SR.Name);
    until FindNext(SR) <> 0;
    FindClose(SR);
  end;

  if FindFirst('/dev/ttyACM*', faAnyFile, SR) = 0 then
  begin
    repeat
      ComboBox1.Items.Add('/dev/' + SR.Name);
    until FindNext(SR) <> 0;
    FindClose(SR);
  end;
  {$ENDIF}

  // Sikrer et pænt fallback, hvis PC'en ikke har noget tilsluttet
  if ComboBox1.Items.Count > 0 then
    ComboBox1.ItemIndex := 0
  else
    ComboBox1.Text := 'Ingen COM-porte fundet';
end;

procedure TWeatherstation.SSIDClick(Sender: TObject);
begin
end;

end.
